#include <vector>
#include <iostream>
#include <string>
#include <curl/curl.h>
#include <curl/easy.h>
#include <curl/curlbuild.h>
using namespace std;

static size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

int main(int argc, const char * argv[]) {
    // insert code here...
    CURL *curl;
    CURLcode res;
    std::string readBuffer;
//    string url = "http://192.168.10.88:3000/channels/1/fields/1/last?key=5PTJZFXQ6SWD32PR";
    curl = curl_easy_init();

    curl_easy_setopt(curl, CURLOPT_URL, "http://192.168.10.88:3000/channels/1/fields/1/last?key=5PTJZFXQ6SWD32PR");
    curl_easy_setopt(curl, CURLOPT_HTTPGET,1);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);

    res = curl_easy_perform(curl);

//    std::cout << readBuffer << std::endl; 
    printf("value is %s",readBuffer.c_str());

    return 0;


}