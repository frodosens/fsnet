#include "fsnet/replication_wire.hpp"
namespace fsnet {
namespace {
void put(std::string& out,std::uint64_t value,unsigned count) { for(unsigned i=count;i;--i) out+=static_cast<char>(value>>((i-1)*8)); }
std::uint64_t read_uint(std::string_view& in,unsigned count) {
    if(in.size()<count) throw std::invalid_argument("truncated replication frame");
    std::uint64_t n=0; for(unsigned i=0;i<count;++i) n=(n<<8)|static_cast<unsigned char>(in[i]); in.remove_prefix(count); return n;
}
}
std::string encode_entity_key(const std::string& type, const std::string& id) {
    if (type.empty() || type.size()>127 || type.find_first_of(".\0",0,2)!=std::string::npos || id.empty() || id.size()>256)
        throw std::invalid_argument("invalid replication entity");
    std::string out; put(out,type.size(),1); out+=type; put(out,id.size(),2); out+=id; return out;
}
std::pair<std::string,std::string> decode_entity_key(std::string_view& in) {
    auto n=read_uint(in,1); if(n>in.size()) throw std::invalid_argument("truncated entity type");
    std::string type(in.substr(0,n)); in.remove_prefix(n);
    n=read_uint(in,2); if(n>in.size()) throw std::invalid_argument("truncated entity id");
    std::string id(in.substr(0,n)); in.remove_prefix(n); (void)encode_entity_key(type,id); return {type,id};
}
std::string encode_replication_packet(std::uint64_t subscription, const std::string& type, const std::string& id, Status status, const AttributeUpdate* update, std::uint32_t lease_ms) {
    std::string out="FSN1"; put(out,static_cast<unsigned>(status),1); put(out,subscription,8); put(out,lease_ms,4); out+=encode_entity_key(type,id);
    if(status==Status::ok) { if(!update) throw std::invalid_argument("missing property update"); out+=encode_attributes(*update); }
    return out;
}
ReplicationPacket decode_replication_packet(std::string_view in) {
    if(in.size()>1024*1024 || !in.starts_with("FSN1")) throw std::invalid_argument("invalid replication packet");
    in.remove_prefix(4); auto status=read_uint(in,1); if(status>static_cast<unsigned>(Status::internal)) throw std::invalid_argument("invalid replication status");
    auto subscription=read_uint(in,8); if(!subscription) throw std::invalid_argument("invalid subscription");
    auto lease_ms=static_cast<std::uint32_t>(read_uint(in,4));
    auto [type,id]=decode_entity_key(in); ReplicationPacket result{subscription,std::move(type),std::move(id),static_cast<Status>(status),lease_ms,{}};
    if(result.status==Status::ok) result.update=decode_attributes(in);
    else if(!in.empty()) throw std::invalid_argument("trailing revocation bytes");
    return result;
}
}
