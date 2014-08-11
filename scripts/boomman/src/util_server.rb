

require 'http_server.rb'
require 'json'
class UtilServer < HTTPServer
	
	attr_reader :cache_data
	attr_reader :redir_path
	
	def initialize(*args)
		super(*args)
		@cache_data = {}
		@redir_path = {
			
			"/" => "/index.html" 
			
		}
	end
	
	# get 請求
	def on_handle_request(request)
		
		# 先嘗試執行函數
		begin
			m = method("handle_" + request.uri[1, request.uri.size]);
			return m.call(request)
		rescue => err
    	response = handle_file(request);
		end
		return response;
	end
	
	
	# 請求文件
	def handle_file(request)
		
		if(@redir_path[request.uri].nil?)
			file_name = request.uri
		else
			file_name = @redir_path[request.uri]	
		end
		
		path = "res/html#{file_name}"
		
		begin
			
			if(@cache_data[path].nil?)
		
				file = File.open(path)
				@cache_data[path] = file.read()
				file.close()
		
			end
	
	    response = HTTPResponse.new(200, @cache_data[path]);
			response.set_header("Content-Type", "text/html; charset=UTF-8")
			return response;
		
		rescue => err
		
			noexist = HTTPResponse.new(404, "出錯了喔 GET #{request.uri} #{err.message}")
			return noexist;
		
		end
	end
	
	
	# 清除緩存
	def handle_clear_cache(request)

		s1 = Time.now.tv_usec
		CMDSycnFile.clear_cache
		@cache_data.clear();
		s2 = Time.now.tv_usec
		
		
    response = HTTPResponse.new(200, "緩存清理完畢 (#{s2 - s1} us)");
		return response;
		
	end
	
	
end