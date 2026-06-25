#include "miniblink_host/frame/mb_cache_storage.h"

#include <stdint.h>

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "base/types/expected.h"
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

class MbCacheStorageCache : public m::CacheStorageCache {
 public:
  explicit MbCacheStorageCache(MbCacheData* data) : data_(data) {}

  void Match(m::FetchAPIRequestPtr request, m::CacheQueryOptionsPtr, bool, bool,
             int64_t, MatchCallback callback) override {
    auto it = data_->entries.find(UrlKey(request));
    if (it == data_->entries.end()) {
      std::move(callback).Run(base::unexpected(m::CacheStorageError::kErrorNotFound));
      return;
    }
    std::move(callback).Run(
        m::MatchResponse::NewResponse(CloneStored(it->second)));
  }

  // matchAll(request?): with a request, the matching response (0/1 by URL); without one,
  // every cached response, in URL order.
  void MatchAll(m::FetchAPIRequestPtr request, m::CacheQueryOptionsPtr, int64_t,
                MatchAllCallback callback) override {
    blink::Vector<m::FetchAPIResponsePtr> out;
    if (request) {
      auto it = data_->entries.find(UrlKey(request));
      if (it != data_->entries.end())
        out.push_back(CloneStored(it->second));
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
  // keys(request?): the cached requests (filtered to one by URL if given).
  void Keys(m::FetchAPIRequestPtr request, m::CacheQueryOptionsPtr, int64_t,
            KeysCallback callback) override {
    blink::Vector<m::FetchAPIRequestPtr> out;
    if (request) {
      auto it = data_->entries.find(UrlKey(request));
      if (it != data_->entries.end())
        out.push_back(MakeRequest(it->first));
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
        data_->entries.erase(key);
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
  void Has(const blink::String& cache_name, int64_t,
           HasCallback callback) override {
    std::move(callback).Run(CacheRegistry().count(cache_name.Utf8())
                                ? m::CacheStorageError::kSuccess
                                : m::CacheStorageError::kErrorCacheNameNotFound);
  }
  void Delete(const blink::String& cache_name, int64_t,
              DeleteCallback callback) override {
    bool existed = CacheRegistry().erase(cache_name.Utf8()) != 0;
    std::move(callback).Run(existed
                                ? m::CacheStorageError::kSuccess
                                : m::CacheStorageError::kErrorCacheNameNotFound);
  }
  void Keys(int64_t, KeysCallback callback) override {
    blink::Vector<blink::String> names;
    for (const auto& cache : CacheRegistry())
      names.push_back(blink::String::FromUtf8(std::string_view(cache.first)));
    std::move(callback).Run(names);
  }
  void Match(m::FetchAPIRequestPtr request, m::MultiCacheQueryOptionsPtr, bool,
             bool, int64_t, MatchCallback callback) override {
    std::string key = UrlKey(request);
    for (auto& cache : CacheRegistry()) {
      auto it = cache.second->entries.find(key);
      if (it != cache.second->entries.end()) {
        std::move(callback).Run(
            m::MatchResponse::NewResponse(CloneStored(it->second)));
        return;
      }
    }
    std::move(callback).Run(base::unexpected(m::CacheStorageError::kErrorNotFound));
  }
  void Open(const blink::String& cache_name, int64_t,
            OpenCallback callback) override {
    std::string key = cache_name.Utf8();
    auto& slot = CacheRegistry()[key];
    if (!slot)
      slot = std::make_unique<MbCacheData>();
    mojo::PendingAssociatedRemote<m::CacheStorageCache> remote;
    mojo::MakeSelfOwnedAssociatedReceiver(
        std::make_unique<MbCacheStorageCache>(slot.get()),
        remote.InitWithNewEndpointAndPassReceiver());
    std::move(callback).Run(std::move(remote));
  }
};

}  // namespace

void BindCacheStorage(
    mojo::PendingReceiver<blink::mojom::blink::CacheStorage> receiver) {
  mojo::MakeSelfOwnedReceiver(std::make_unique<MbCacheStorage>(),
                              std::move(receiver));
}

}  // namespace mb
