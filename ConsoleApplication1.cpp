#include <iostream>
#include <fstream>
#include <vector>
#include <queue>
#include <map>
#include <set>
#include <string>
#include <curl/curl.h>
#include <gumbo.h>
#include <regex>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <functional>
#include <algorithm>
#include <unordered_set>
#include <cctype>
#include <urlmon.h>
#include <iomanip>

#pragma comment(lib, "urlmon.lib")

using namespace std;

mutex queue_mutex, file_mutex, domains_mutex, set_mutex;
queue<pair<string, int>> url_queue;
set<string> visited_urls;
unordered_set<string> visited_domains;
map<string, int> domain_max_depth;
ofstream output_file;
atomic<int> pages_downloaded(0);

const int MAX_PAGES = 10000000;
const int MAX_DEPTH_PER_DOMAIN = 3;
const int MAX_DOMAIN_HOPS = 10;
const int NUM_THREADS = 12;

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}


string download_page(const string& url) {
    CURL* curl = curl_easy_init();
    string response;

    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (compatible; SmartCrawler/1.0)");
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            cerr << "Failed to download: " << url << " - " << curl_easy_strerror(res) << endl;
            response = "";
        }

        curl_easy_cleanup(curl);
    }

    return response;
}

string normalize_url(const string& url, const string& base_url = "") {
    string result = url;

    size_t pos = result.find('?');
    if (pos != string::npos) result = result.substr(0, pos);

    pos = result.find('#');
    if (pos != string::npos) result = result.substr(0, pos);

    transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return tolower(c);
        });

    if (result.size() > 1 && result.back() == '/') {
        result.pop_back();
    }

    if (!base_url.empty() && result.find("http") != 0) {

        if (!result.empty() && result[0] == '/') {

            size_t protocol_end = base_url.find("://");
            if (protocol_end != string::npos) {
                size_t domain_end = base_url.find('/', protocol_end + 3);
                if (domain_end != string::npos) {
                    result = base_url.substr(0, domain_end) + result;
                }
                else {
                    result = base_url + result;
                }
            }
        }
 
        else {

            size_t last_slash = base_url.find_last_of('/');
            if (last_slash != string::npos && last_slash > base_url.find("://") + 2) {
   
                result = base_url.substr(0, last_slash + 1) + result;
            }
            else {
                result = base_url + "/" + result;
            }
        }
    }


    if (result.find("://") == string::npos) {
        result = "http://" + result;
    }

    size_t pos_dsl = result.find("://");
    if (pos_dsl != string::npos) {
        string protocol = result.substr(0, pos_dsl + 3);
        string path = result.substr(pos_dsl + 3);

        while (path.find("//") != string::npos) {
            path.replace(path.find("//"), 2, "/");
        }

        result = protocol + path;
    }

    return result;
}


string extract_domain(const string& url) {
    regex domain_regex(R"(^(?:https?:\/\/)?(?:www\.)?([^\/:?#]+)(?:[\/:?#]|$))", regex::icase);
    smatch match;
    if (regex_search(url, match, domain_regex)) {
        string domain = match[1].str();


        transform(domain.begin(), domain.end(), domain.begin(), [](unsigned char c) {
            return tolower(c);
            });

        return domain;
    }
    return "";
}


string get_base_path(const string& url) {
    size_t pos = url.find("://");
    if (pos == string::npos) return "";

    pos = url.find('/', pos + 3);
    if (pos == string::npos) return url + "/";

    return url.substr(0, pos + 1);
}

vector<string> extract_links(const string& html, const string& base_url) {
    vector<string> links;
    GumboOutput* output = gumbo_parse(html.c_str());

    if (!output || !output->root) {
        return links;
    }

    std::function<void(GumboNode*)> find_links = [&](GumboNode* node) {
        if (node->type != GUMBO_NODE_ELEMENT) return;

        if (node->v.element.tag == GUMBO_TAG_A) {
            GumboAttribute* href = gumbo_get_attribute(&node->v.element.attributes, "href");
            if (href && href->value) {
                string link = href->value;

                if (link.empty()) return;

                if (link[0] == '#') return;

                // Нормализация ссылки
                link = normalize_url(link, base_url);

                if (link.find("mailto:") != string::npos ||
                    link.find("javascript:") != string::npos ||
                    link.find("tel:") != string::npos ||
                    link.find(".pdf") != string::npos ||
                    link.find(".jpg") != string::npos ||
                    link.find(".png") != string::npos) {
                    return;
                }

                links.push_back(link);
            }
        }

        GumboVector* children = &node->v.element.children;
        for (unsigned int i = 0; i < children->length; ++i) {
            find_links(static_cast<GumboNode*>(children->data[i]));
        }
        };

    find_links(output->root);
    gumbo_destroy_output(&kGumboDefaultOptions, output);

    sort(links.begin(), links.end());
    auto last = unique(links.begin(), links.end());
    links.erase(last, links.end());

    return links;
}

void worker_thread() {
    while (pages_downloaded < MAX_PAGES) {
        pair<string, int> url_item;
        string current_url;
        int current_depth;

        {
            lock_guard<mutex> lock(queue_mutex);
            if (url_queue.empty()) {
                this_thread::sleep_for(chrono::milliseconds(100));
                continue;
            }
            url_item = url_queue.front();
            url_queue.pop();
            current_url = url_item.first;
            current_depth = url_item.second;
        }

        string domain = extract_domain(current_url);
        if (domain.empty()) continue;

        bool skip = false;
        {
            lock_guard<mutex> lock(domains_mutex);

            if (visited_domains.find(domain) == visited_domains.end()) {
                visited_domains.insert(domain);
                domain_max_depth[domain] = current_depth;
            }
            else if (current_depth > domain_max_depth[domain] + MAX_DEPTH_PER_DOMAIN) {
                skip = true;
            }
            else if (current_depth > domain_max_depth[domain]) {
                domain_max_depth[domain] = current_depth;
            }
        }

        if (skip) {
            continue;
        }
        {
            lock_guard<mutex> lock(set_mutex);
            if (visited_urls.find(current_url) != visited_urls.end()) {
                continue;
            }
            visited_urls.insert(current_url);
        }

        string html = download_page(current_url);
        if (html.empty()) continue;

        {
            lock_guard<mutex> lock(file_mutex);
            output_file << current_url << endl;
        }

        vector<string> links = extract_links(html, current_url);

        {
            lock_guard<mutex> lock(queue_mutex);
            for (const string& link : links) {
                string new_domain = extract_domain(link);
                if (new_domain.empty()) continue;

                int new_depth = current_depth;

                bool same_domain = (new_domain == domain);
                bool new_domain_flag = false;

                {
                    lock_guard<mutex> lock_d(domains_mutex);
                    new_domain_flag = (visited_domains.find(new_domain) == visited_domains.end());
                }

                if (new_domain_flag) {

                    if (visited_domains.size() >= MAX_DOMAIN_HOPS) {
                        continue;
                    }
                    new_depth = 0;
                }
                else if (!same_domain) {
                    new_depth = current_depth + 1;
                }
                else {
                    new_depth = current_depth + 1;
                }

                if (new_depth > MAX_DEPTH_PER_DOMAIN && same_domain) {
                    continue;
                }

                {
                    lock_guard<mutex> lock_s(set_mutex);
                    if (visited_urls.find(link) != visited_urls.end()) {
                        continue;
                    }
                }

                url_queue.push({ link, new_depth });
            }
        }

        int count = ++pages_downloaded;
        cout << "[" << count << "/" << MAX_PAGES << "] "
            << "Domain: " << left << setw(20) << domain
            << " Depth: " << current_depth
            << " URL: " << current_url << endl;

        if (count >= MAX_PAGES) break;
    }
}



int main() {
    setlocale(LC_ALL, "ru");
    const string start_url = "https://localhost";

    output_file.open("sites.txt", ios::app);

    string start_domain = extract_domain(start_url);
    if (start_domain.empty()) {
        cerr << "Invalid start URL" << endl;
        return 1;
    }

    {
        lock_guard<mutex> lock(domains_mutex);
        visited_domains.insert(start_domain);
        domain_max_depth[start_domain] = 0;
    }

    url_queue.push({ normalize_url(start_url), 0 });

    vector<thread> threads;
    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back(worker_thread);
    }

    for (auto& t : threads) {
        t.join();
    }

    output_file.close();

    cout << "\nCrawling completed!" << endl;
    cout << "Total domains visited: " << visited_domains.size() << endl;
    cout << "Total pages downloaded: " << pages_downloaded << endl;
    cout << "Results saved to sites.txt" << endl;

    return 0;
}