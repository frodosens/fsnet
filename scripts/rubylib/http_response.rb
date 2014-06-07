


class HTTPResponse
    
    attr_accessor :code;
    attr_accessor :data;
    attr_reader   :headers;
    
    
    def initialize(code, data)
        @code = code;
        @data = data;
        @headers = { "Server" => "HTTPServer",
                     "Content-Type" => "text/plain; charset=UTF-8",
                     "Connection"   =>  "keepalive"
                    };
    end
    
    def add_header(key, value)
        @headers[key] = value
    end
end