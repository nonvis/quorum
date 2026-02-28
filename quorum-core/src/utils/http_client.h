#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

#include <curl/curl.h>

namespace sui::quorum {

struct HttpResponse {
    int status_code{0};
    std::string body;
    std::string error;

    [[nodiscard]] bool success() const {
        return status_code >= 200 && status_code < 300;
    }
};

class HttpClient {
public:
    HttpClient() {
        curl_ = curl_easy_init();
    }

    ~HttpClient() {
        if (curl_) curl_easy_cleanup(curl_);
    }

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;
    HttpClient(HttpClient&&) = delete;
    HttpClient& operator=(HttpClient&&) = delete;

    void set_timeout(std::chrono::milliseconds timeout) {
        timeout_ = timeout;
    }

    void set_user_agent(std::string_view ua) {
        user_agent_ = std::string(ua);
    }

    [[nodiscard]] HttpResponse get(const std::string& url) {
        return perform(url, nullptr, {});
    }

    [[nodiscard]] HttpResponse post_json(const std::string& url, const std::string& json_body) {
        return perform(url, json_body.c_str(), {"Content-Type: application/json"});
    }

    [[nodiscard]] HttpResponse post_json(const std::string& url, const std::string& json_body,
                                          const std::vector<std::string>& extra_headers) {
        std::vector<std::string> headers = {"Content-Type: application/json"};
        headers.insert(headers.end(), extra_headers.begin(), extra_headers.end());
        return perform(url, json_body.c_str(), headers);
    }

private:
    CURL* curl_{nullptr};
    std::chrono::milliseconds timeout_{15000};
    std::string user_agent_{"quorum-daemon/0.1.0"};

    static size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
        auto* response = static_cast<std::string*>(userdata);
        response->append(ptr, size * nmemb);
        return size * nmemb;
    }

    HttpResponse perform(const std::string& url, const char* post_data,
                          const std::vector<std::string>& extra_headers) {
        HttpResponse resp;
        if (!curl_) {
            resp.error = "CURL not initialized";
            return resp;
        }

        curl_easy_reset(curl_);
        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &resp.body);
        curl_easy_setopt(curl_, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_.count()));
        curl_easy_setopt(curl_, CURLOPT_USERAGENT, user_agent_.c_str());
        curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 1L);

        struct curl_slist* headers = nullptr;
        for (const auto& h : extra_headers) {
            headers = curl_slist_append(headers, h.c_str());
        }
        if (headers) {
            curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);
        }

        if (post_data) {
            curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, post_data);
        }

        CURLcode res = curl_easy_perform(curl_);
        if (res != CURLE_OK) {
            resp.error = curl_easy_strerror(res);
        } else {
            long code = 0;
            curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &code);
            resp.status_code = static_cast<int>(code);
        }

        if (headers) curl_slist_free_all(headers);
        return resp;
    }
};

} // namespace sui::quorum
