


class HTTPRequest
    
    attr_reader :method;
    attr_reader :uri;
    attr_reader :params;
    attr_reader :headers;
    attr_reader :post_stream;
    
    attr_reader :magic_request_id;
    
    class << self
        def parse(pack)
            
            magic_request_id = pack.read_data.read_uint64;
            
            req_method = pack.read_data.read_string;
            uri = pack.read_data.read_string;
            len = pack.read_data.read_int16;
            data = pack.read_data.read_data(len);
            params_count = pack.read_data.read_int16();
            params = {};
            headres = {};
            for i in 0...params_count
                key = pack.read_data.read_string;
                value = pack.read_data.read_string;
                params[key] = value;
            end
            
            header_count = pack.read_data.read_int16;
            for i in 0...header_count
                key = pack.read_data.read_string;
                value = pack.read_data.read_string;
                headres[key] = value;
            end
            
            post_stream = FSInputStream.new( data, len )
            
            ret = HTTPRequest.new(magic_request_id, req_method, uri, params, headres, post_stream);
            return ret;
        end
    
    end
    
    def initialize(magic_request_id, method, uri, params, headers, post_stream)
        @magic_request_id = magic_request_id
        @method = method;
        @uri = uri;
        @params = params;
        @headers = headers;
        @post_stream = post_stream;
        
    end

    
end