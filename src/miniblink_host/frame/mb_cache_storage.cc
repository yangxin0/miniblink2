#include "miniblink_host/frame/mb_cache_storage.h"

#include <stdint.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/types/expected.h"
#include "miniblink_host/frame/mb_frame_origin.h"
#include "mojo/public/cpp/bindings/pending_associated_remote.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_associated_receiver.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "third_party/blink/public/mojom/fetch/fetch_api_request.mojom-blink.h"
#include "third_party/blink/public/mojom/fetch/fetch_api_response.mojom-blink.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"

namespace mb {
namespace {

namespace m = blink::mojom::blink;

// A cache holds an ordered list of (request, response) entries. In the blink variant a
// Response body is a refcounted scoped_refptr<BlobDataHandle>, so Clone() shares it — an
// entry can be matched any number of times. The *request* cannot be Clone()d (its
// ResourceRequestBody field has no clone), so per entry we keep the URL and the request
// headers (needed for Vary matching) and rebuild a minimal request for cache.keys().
//
// Entries are kept in insertion order (the Cache API requires insertion order for
// matchAll()/keys() and first-inserted for match()), and multiple entries may share a URL
// when they differ on the request headers named by the response's Vary header.
struct MbCacheEntry {
  std::string url;
  std::map<std::string, std::string> request_headers;  // lowercased name -> value
  m::FetchAPIResponsePtr response;
};

struct MbCacheData {
  std::vector<MbCacheEntry> entries;  // insertion order
};

// CacheRegistry owns the data via shared_ptr so a Cache handle (MbCacheStorageCache) obtained
// from caches.open() keeps the data alive even after caches.delete() removes the registry
// entry — avoiding a use-after-free while the JS handle is still in use.
std::map<std::string, std::shared_ptr<MbCacheData>>& CacheRegistry() {
  static auto* r = new std::map<std::string, std::shared_ptr<MbCacheData>>();
  return *r;
}

m::FetchAPIResponsePtr CloneStored(const m::FetchAPIResponsePtr& resp) {
  return resp.Clone();
}

std::string UrlKey(const m::FetchAPIRequestPtr& request) {
  return request ? request->url.GetString().Utf8() : std::string();
}

std::string LowerAscii(std::string s) {
  for (char& c : s)
    c = (c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c;
  return s;
}

// Request headers as a lowercased-name -> value map (HTTP header names are case-insensitive),
// for Vary matching.
std::map<std::string, std::string> RequestHeaders(
    const m::FetchAPIRequestPtr& request) {
  std::map<std::string, std::string> out;
  if (request)
    for (const auto& h : request->headers)
      out[LowerAscii(h.key.Utf8())] = h.value.Utf8();
  return out;
}

// The response's Vary header value ("" if absent), looked up case-insensitively.
std::string ResponseVary(const m::FetchAPIResponsePtr& response) {
  if (response)
    for (const auto& h : response->headers)
      if (LowerAscii(h.key.Utf8()) == "vary")
        return h.value.Utf8();
  return std::string();
}

// A minimal GET request carrying the URL (and the stored request headers) — enough for
// cache.keys() to hand back Request objects the page can read .url/.headers from.
m::FetchAPIRequestPtr MakeRequest(const MbCacheEntry& entry) {
  auto req = m::FetchAPIRequest::New();
  req->url = blink::KURL(blink::String::FromUtf8(entry.url));
  req->method = "GET";
  for (const auto& h : entry.request_headers)
    req->headers.insert(blink::String::FromUtf8(h.first),
                        blink::String::FromUtf8(h.second));
  return req;
}

// Drop the query (?...) and fragment (#...) so {ignoreSearch:true} matches a URL regardless of
// its query string.
std::string StripSearch(std::string url) {
  if (auto h = url.find('#'); h != std::string::npos)
    url.resize(h);
  if (auto q = url.find('?'); q != std::string::npos)
    url.resize(q);
  return url;
}

// Does a stored entry's URL match the query URL under the request's ignoreSearch option?
bool KeyMatches(const std::string& stored,
                const std::string& target,
                bool ignore_search) {
  return ignore_search ? StripSearch(stored) == StripSearch(target)
                       : stored == target;
}

// Cache API Vary matching: the entry matches `query_headers` if, for every field named in the
// entry's response Vary header, the query request and the entry's stored request agree (a "*"
// field never matches). Returns true when the response has no Vary header.
bool VaryMatches(const std::map<std::string, std::string>& query_headers,
                 const MbCacheEntry& entry) {
  const std::string vary = ResponseVary(entry.response);
  if (vary.empty())
    return true;
  for (size_t pos = 0; pos < vary.size();) {
    size_t comma = vary.find(',', pos);
    if (comma == std::string::npos)
      comma = vary.size();
    std::string field = vary.substr(pos, comma - pos);
    pos = comma + 1;
    size_t b = field.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
      continue;  // empty field, skip
    size_t e = field.find_last_not_of(" \t\r\n");
    field = LowerAscii(field.substr(b, e - b + 1));
    if (field == "*")
      return false;
    auto qi = query_headers.find(field);
    auto si = entry.request_headers.find(field);
    const std::string qv =
        qi == query_headers.end() ? std::string() : qi->second;
    const std::string sv =
        si == entry.request_headers.end() ? std::string() : si->second;
    if (qv != sv)
      return false;
  }
  return true;
}

// Whether an entry matches the query request under the given options.
bool EntryMatches(const MbCacheEntry& entry,
                  const std::string& target_url,
                  const std::map<std::string, std::string>& query_headers,
                  bool ignore_search,
                  bool ignore_vary) {
  if (!KeyMatches(entry.url, target_url, ignore_search))
    return false;
  return ignore_vary || VaryMatches(query_headers, entry);
}

class MbCacheStorageCache : public m::CacheStorageCache {
 public:
  // Holds a shared_ptr (not a raw pointer) so the data survives caches.delete() while this
  // handle is still in use — see CacheRegistry().
  explicit MbCacheStorageCache(std::shared_ptr<MbCacheData> data)
      : data_(std::move(data)) {}

  void Match(m::FetchAPIRequestPtr request, m::CacheQueryOptionsPtr options,
             bool, bool, int64_t, MatchCallback callback) override {
    const bool ignore_search = options && options->ignore_search;
    const bool ignore_vary = options && options->ignore_vary;
    const std::string target = UrlKey(request);
    const auto headers = RequestHeaders(request);
    for (auto& e : data_->entries) {
      if (EntryMatches(e, target, headers, ignore_search, ignore_vary)) {
        std::move(callback).Run(
            m::MatchResponse::NewResponse(CloneStored(e.response)));
        return;
      }
    }
    std::move(callback).Run(base::unexpected(m::CacheStorageError::kErrorNotFound));
  }

  // matchAll(request?): with a request, every response that matches (honoring ignoreSearch and
  // Vary); without one, every cached response, in insertion order.
  void MatchAll(m::FetchAPIRequestPtr request, m::CacheQueryOptionsPtr options,
                int64_t, MatchAllCallback callback) override {
    blink::Vector<m::FetchAPIResponsePtr> out;
    if (request) {
      const bool ignore_search = options && options->ignore_search;
      const bool ignore_vary = options && options->ignore_vary;
      const std::string target = UrlKey(request);
      const auto headers = RequestHeaders(request);
      for (auto& e : data_->entries)
        if (EntryMatches(e, target, headers, ignore_search, ignore_vary))
          out.push_back(CloneStored(e.response));
    } else {
      for (auto& e : data_->entries)
        out.push_back(CloneStored(e.response));
    }
    std::move(callback).Run(std::move(out));
  }
  void GetAllMatchedEntries(m::FetchAPIRequestPtr, m::CacheQueryOptionsPtr,
                            int64_t,
                            GetAllMatchedEntriesCallback callback) override {
    std::move(callback).Run(blink::Vector<m::CacheEntryPtr>());
  }
  // keys(request?): the cached requests in insertion order (filtered by URL + Vary, honoring
  // ignoreSearch, if a request is given).
  void Keys(m::FetchAPIRequestPtr request, m::CacheQueryOptionsPtr options,
            int64_t, KeysCallback callback) override {
    blink::Vector<m::FetchAPIRequestPtr> out;
    if (request) {
      const bool ignore_search = options && options->ignore_search;
      const bool ignore_vary = options && options->ignore_vary;
      const std::string target = UrlKey(request);
      const auto headers = RequestHeaders(request);
      for (auto& e : data_->entries)
        if (EntryMatches(e, target, headers, ignore_search, ignore_vary))
          out.push_back(MakeRequest(e));
    } else {
      for (auto& e : data_->entries)
        out.push_back(MakeRequest(e));
    }
    std::move(callback).Run(std::move(out));
  }

  void Batch(blink::Vector<m::BatchOperationPtr> batch_operations, int64_t,
             BatchCallback callback) override {
    // blink resolves cache.delete() to false only when the batch reports kErrorNotFound, so a
    // delete op that matched nothing must surface that error.
    bool delete_matched_nothing = false;
    for (auto& op : batch_operations) {
      const std::string key = UrlKey(op->request);
      auto& v = data_->entries;
      if (op->operation_type == m::OperationType::kDelete) {
        // delete(request, options) removes every matching entry (honoring ignoreSearch/
        // ignoreVary).
        const bool ignore_search =
            op->match_options && op->match_options->ignore_search;
        const bool ignore_vary =
            op->match_options && op->match_options->ignore_vary;
        const auto headers = RequestHeaders(op->request);
        const size_t before = v.size();
        v.erase(std::remove_if(v.begin(), v.end(),
                               [&](const MbCacheEntry& e) {
                                 return EntryMatches(e, key, headers,
                                                     ignore_search, ignore_vary);
                               }),
                v.end());
        if (v.size() == before)
          delete_matched_nothing = true;
        continue;
      }
      if (op->operation_type != m::OperationType::kPut || !op->response)
        continue;
      // Per spec, a put first removes existing entries that match the request by URL + Vary
      // (default options) so re-putting replaces, while a differing Vary keeps both.
      auto headers = RequestHeaders(op->request);
      v.erase(std::remove_if(v.begin(), v.end(),
                             [&](const MbCacheEntry& e) {
                               return EntryMatches(e, key, headers,
                                                   /*ignore_search=*/false,
                                                   /*ignore_vary=*/false);
                             }),
              v.end());
      MbCacheEntry entry;
      entry.url = key;
      entry.request_headers = std::move(headers);
      entry.response = std::move(op->response);
      v.push_back(std::move(entry));
    }
    std::move(callback).Run(m::CacheStorageVerboseError::New(
        delete_matched_nothing ? m::CacheStorageError::kErrorNotFound
                               : m::CacheStorageError::kSuccess,
        blink::String()));
  }

  void WriteSideData(const blink::KURL&, base::Time, mojo_base::BigBuffer, int64_t,
                     WriteSideDataCallback callback) override {
    std::move(callback).Run(m::CacheStorageError::kSuccess);
  }

 private:
  std::shared_ptr<MbCacheData> data_;
};

class MbCacheStorage : public m::CacheStorage {
 public:
  // `frame_key` -> the frame's origin: cache names are stored under a per-origin
  // prefix so caches.open('v1') in different origins are ISOLATED (the registry
  // was keyed by bare name -> cross-origin cache data sharing). 0 = unknown origin.
  explicit MbCacheStorage(uint64_t frame_key) : frame_key_(frame_key) {}

  void Has(const blink::String& cache_name, int64_t,
           HasCallback callback) override {
    std::move(callback).Run(CacheRegistry().count(Key(cache_name))
                                ? m::CacheStorageError::kSuccess
                                : m::CacheStorageError::kErrorCacheNameNotFound);
  }
  void Delete(const blink::String& cache_name, int64_t,
              DeleteCallback callback) override {
    bool existed = CacheRegistry().erase(Key(cache_name)) != 0;
    std::move(callback).Run(existed
                                ? m::CacheStorageError::kSuccess
                                : m::CacheStorageError::kErrorCacheNameNotFound);
  }
  void Keys(int64_t, KeysCallback callback) override {
    blink::Vector<blink::String> names;
    const std::string scope = Scope();
    for (const auto& cache : CacheRegistry())
      if (cache.first.rfind(scope, 0) == 0)  // this origin's caches only
        names.push_back(
            blink::String::FromUtf8(cache.first.substr(scope.size())));
    std::move(callback).Run(names);
  }
  // caches.match(request, options): search this origin's caches (or just
  // options.cache_name) for the first matching entry, honoring ignoreSearch.
  void Match(m::FetchAPIRequestPtr request, m::MultiCacheQueryOptionsPtr options,
             bool, bool, int64_t, MatchCallback callback) override {
    const std::string target = UrlKey(request);
    const bool ignore_search =
        options && options->query_options &&
        options->query_options->ignore_search;
    const bool ignore_vary =
        options && options->query_options &&
        options->query_options->ignore_vary;
    const auto headers = RequestHeaders(request);
    const std::string scope = Scope();
    const std::string only_cache =
        options && !options->cache_name.IsNull() ? Key(options->cache_name)
                                                 : std::string();
    for (auto& cache : CacheRegistry()) {
      if (cache.first.rfind(scope, 0) != 0)  // skip other origins' caches
        continue;
      if (!only_cache.empty() && cache.first != only_cache)
        continue;
      for (auto& e : cache.second->entries) {
        if (EntryMatches(e, target, headers, ignore_search, ignore_vary)) {
          std::move(callback).Run(
              m::MatchResponse::NewResponse(CloneStored(e.response)));
          return;
        }
      }
    }
    std::move(callback).Run(base::unexpected(m::CacheStorageError::kErrorNotFound));
  }
  void Open(const blink::String& cache_name, int64_t,
            OpenCallback callback) override {
    auto& slot = CacheRegistry()[Key(cache_name)];
    if (!slot)
      slot = std::make_shared<MbCacheData>();
    mojo::PendingAssociatedRemote<m::CacheStorageCache> remote;
    mojo::MakeSelfOwnedAssociatedReceiver(
        std::make_unique<MbCacheStorageCache>(slot),
        remote.InitWithNewEndpointAndPassReceiver());
    std::move(callback).Run(std::move(remote));
  }

 private:
  // Per-origin prefix for cache-name registry keys (SEP can't appear in an origin
  // or a cache name). For a bucket, frame_key maps to "origin\x01bucket:name".
  std::string Scope() const { return MbGetFrameOrigin(frame_key_) + "\x01"; }
  std::string Key(const blink::String& cache_name) const {
    return Scope() + cache_name.Utf8();
  }
  uint64_t frame_key_ = 0;
};

}  // namespace

void BindCacheStorage(
    mojo::PendingReceiver<blink::mojom::blink::CacheStorage> receiver,
    uint64_t frame_key) {
  mojo::MakeSelfOwnedReceiver(std::make_unique<MbCacheStorage>(frame_key),
                              std::move(receiver));
}

// Drop every cache stored under `scope_origin` (an origin, or an "origin\x01bucket:name"
// bucket scope) — i.e. all registry entries prefixed by that scope. Used by Storage Buckets'
// deleteBucket() to empty the bucket's Cache Storage. Any live Cache handle keeps working on
// detached data (it holds a shared_ptr), so this cannot dangle.
void MbClearCacheStorageForScope(const std::string& scope_origin) {
  const std::string prefix = scope_origin + "\x01";
  auto& reg = CacheRegistry();
  for (auto it = reg.begin(); it != reg.end();) {
    if (it->first.rfind(prefix, 0) == 0)
      it = reg.erase(it);
    else
      ++it;
  }
}

}  // namespace mb
