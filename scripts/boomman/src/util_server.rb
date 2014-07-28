

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
	
	# 在綫玩家列表
	def handle_online_playes(request)
		
		result = {}
		
		for k, v in $game_players.players
			result[k] = [v.level, v.sex, v.name]
		end
		
    response = HTTPResponse.new(200, result.to_json);
		response.set_header("Content-Type", "text/json; charset=UTF-8")
		return response;
	end
	
	# 請求刷新資源
	def handle_refresh_res(request)
		s1 = Time.now.tv_usec
		$game.relose_resource
		s2 = Time.now.tv_usec
		
    response = HTTPResponse.new(200, "重新加載資源完畢 #{Time.now.to_s} (#{s2 - s1} us)");
		return response;
	end
	
	# 重啟服務器
	def handle_reset_server(request)
		
		$game.reset()
		
    response = HTTPResponse.new(200, "重啟完畢");
		return response;	
		
	end
	
	# 請求刷新腳本
	def handle_refresh_script(request)

		s1 = Time.now.tv_usec
		
		load 'db_server.rb'
		load "gate_server.rb"
		load "run_server.rb"
		load "login_server.rb"
		load "util_server.rb"
		Dir['/src/manager/*.rb'].each {|file| load file } 
		Dir['/src/cmds/*.rb'].each {|file| load file }
		Dir['/src/utils/*.rb'].each {|file| load file } 
		Dir['/src/modules/*.rb'].each {|file| load file }
		Dir['/src/modules/templete/*.rb'].each {|file| load file }
		Dir['/src/ext/*.rb'].each {|file| load file }
		Dir['/src/ext/*/*.rb'].each {|file| load file }
		

		s2 = Time.now.tv_usec
		
    response = HTTPResponse.new(200, "重新加載腳本完畢 #{Time.now.to_s} (#{s2 - s1} us)");
		return response;
	end
	
end