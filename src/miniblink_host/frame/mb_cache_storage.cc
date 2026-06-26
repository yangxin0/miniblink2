#include "miniblink_host/frame/mb_cache_storage.h"

#include <stdint.h>

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

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

// A cache holds Request URL -> Response. In the blink variant a Response body is a
// refcounted scoped_refptr<BlobDataHandle>, so Clone() shares it — an entry can be matched
// any number of times. The *request* cannot be Clone()d (its ResourceRequestBody field has
// no clone), so we keep only the URL and rebuild a minimal request for cache.keys().
struct MbCacheData {
  std::map<std::string, m::FetchAPIResponsePtr> entries;  // request URL -> response
};

std::map<std::string, std::unique_ptr<MbCacheData>>& CacheRegistry() {
  static auto* r = new std::map<std::string, std::unique_ptr<MbCacheData>>();
  return *r;
}

m::FetchAPIResponsePtr CloneStored(const m::FetchAPIResponsePtr& resp) {
  return resp.Clone();
}

std::string UrlKey(const m::FetchAPIRequestPtr& request) {
  return request ? request->url.GetString().Utf8() : std::string();
}

// A minimal GET request carrying just the URL — enough for cache.keys() to hand back
// Request objects the page can read .url from.
m::FetchAPIRequestPtr MakeRequest(const std::string& url) {
  auto req = m::FetchAPIRequest::New();
  req->url = blink::KURL(blink::String::FromUtf8(url));
  req->method = "GET";
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

// Does a stored entry's URL match the query URL under the request's options? Method and Vary
// are always ignored here (we don't store either), so only ignoreSearch changes the result.
bool KeyMatches(const std::string& stored,
                const std::string& target,
                bool ignore_search) {
  return ignore_search ? StripSearch(stored) == StripSearch(target)
                       : stored == target;
}

class MbCacheStorageCache : public m::CacheStorageCache {
 public:
  explicit MbCacheStorageCache(MbCacheData* data) : data_(data) {}

  void Match(m::FetchAPIRequestPtr request, m::CacheQueryOptionsPtr options,
             bool, bool, int64_t, MatchCallback callback) override {
    const bool ignore_search = options && options->ignore_search;
    const std::string target = UrlKey(request);
    for (auto& e : data_->entries) {
      if (KeyMatches(e.first, target, ignore_search)) {
        std::move(callback).Run(
            m::MatchResponse::NewResponse(CloneStored(e.second)));
        return;
      }
    }
    std::move(callback).Run(base::unexpected(m::CacheStorageError::kErrorNotFound));
  }

  // matchAll(request?): with a request, every response whose URL matches (honoring
  // ignoreSearch); without one, every cached response, in URL order.
  void MatchAll(m::FetchAPIRequestPtr request, m::CacheQueryOptionsPtr options,
                int64_t, MatchAllCallback callback) override {
    blink::Vector<m::FetchAPIResponsePtr> out;
    if (request) {
      const bool ignore_search = options && options->ignore_search;
      const std::string target = UrlKey(request);
      for (auto& e : data_->entries)
        if (KeyMatches(e.first, target, ignore_search))
          out.push_back(CloneStored(e.second));
    } else {
      for (auto& e : data_->entries)
        out.push_back(CloneStored(e.second));
    }
    std::move(callback).Run(std::move(out));
  }
  void GetAllMatchedEntries(m::FetchAPIRequestPtr, m::CacheQueryOptionsPtr,
                            int64_t,
                            GetAllMatchedEntriesCallback callback) override {
    std::move(callback).Run(blink::Vector<m::CacheEntryPtr>());
  }
  // keys(request?): the cached requests (filtered by URL, honoring ignoreSearch, if given).
  void Keys(m::FetchAPIRequestPtr request, m::CacheQueryOptionsPtr options,
            int64_t, KeysCallback callback) override {
    blink::Vector<m::FetchAPIRequestPtr> out;
    if (request) {
      const bool ignore_search = options && options->ignore_search;
      const std::string target = UrlKey(request);
      for (auto& e : data_->entries)
        if (KeyMatches(e.first, target, ignore_search))
          out.push_back(MakeRequest(e.first));
    } else {
      for (auto& e : data_->entries)
        out.push_back(MakeRequest(e.first));
    }
    std::move(callback).Run(std::move(out));
  }

  void Batch(blink::Vector<m::BatchOperationPtr> batch_operations, int64_t,
             BatchCallback callback) override {
    for (auto& op : batch_operations) {
      std::string key = UrlKey(op->request);
      if (op->operation_type == m::OperationType::kDelete) {
        // delete(request, {ignoreSearch}) removes every matching entry.
        const bool ignore_search =
            op->match_options && op->match_options->ignore_search;
        for (auto it = data_->entries.begin(); it != data_->entries.end();) {
          if (KeyMatches(it->first, key, ignore_search))
            it = data_->entries.erase(it);
          else
            ++it;
        }
        continue;
      }
      if (op->operation_type != m::OperationType::kPut || !op->response)
        continue;
      data_->entries[key] = std::move(op->response);
    }
    std::move(callback).Run(
        m::CacheStorageVerboseError::New(m::CacheStorageError::kSuccess, blink::String()));
  }

  void WriteSideData(const blink::KURL&, base::Time, mojo_base::BigBuffer, int64_t,
                     WriteSideDataCallback callback) override {
    std::move(callback).Run(m::CacheStorageError::kSuccess);
  }

 private:
  MbCacheData* data_;
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
        if (KeyMatches(e.first, target, ignore_search)) {
          std::move(callback).Run(
              m::MatchResponse::NewResponse(CloneStored(e.second)));
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
      slot = std::make_unique<MbCacheData>();
    mojo::PendingAssociatedRemote<m::CacheStorageCache> remote;
    mojo::MakeSelfOwnedAssociatedReceiver(
        std::make_unique<MbCacheStorageCache>(slot.get()),
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

}  // namespace mb
