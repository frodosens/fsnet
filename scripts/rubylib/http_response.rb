

class HeaderItem
	
	attr_accessor :key
	attr_accessor :value

	def initialize(k, v)
		@key = k;
		@value = v
	end

	def to_s
		return "#{@key}=#{@value}"
	end

end
	

class HTTPResponse
    
    attr_accessor :code;
    attr_accessor :data;
    attr_reader   :headers;
    
    
    def initialize(code, data)
        @code = code;
        @data = data;
				
				@headers = []
				
				set_header("Server", "fsnet")
				set_header("Content-Type", "text/plain; charset=UTF-8")
				set_header("Connection", "keepalive")
										
    end
		
		def set_cookies(cookies)
			for c in cookies
				@headers << HeaderItem.new("set-cookie", c.to_s)
			end
		end
    
		def set_header(k, v)
			hits = false
			for c in @headers
				if(c.key == k)
					c.value = v
					hits = true
				end
			end
			if(!hits)
				@headers << HeaderItem.new(k, v)
			end
		end
		
		
end