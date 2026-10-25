#include "fsnet/connection.hpp"
#include <openssl/rand.h>
namespace fsnet {
ConnectionRef ConnectionRef::create() {
    unsigned char bytes[16];if(RAND_bytes(bytes,sizeof(bytes))!=1)throw std::runtime_error("connection identity entropy unavailable");
    constexpr char hex[]="0123456789abcdef";std::string id;id.reserve(32);
    for(auto byte:bytes){id+=hex[byte>>4];id+=hex[byte&15];}
    return {std::move(id),1};
}
}
