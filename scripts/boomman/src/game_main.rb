class GameBoomman
	
	attr_reader :server_start_time
	attr_reader :services;
	attr_reader :util_server
	def start(config_file="game_configure.yaml")
		
		begin
			configure_file = File.open(File.dirname(__FILE__) + "/" + config_file);
			$game_configure = YAML.load(configure_file)
			configure_file.close();
		rescue => err
			print(err.message + "\n");
			exit
    end
		  
		# 建立日誌文件
		@logger_file = Logger.new($game_configure["log_name"])
		@services = []
		@server_start_time = Time.now
		
    # 重啟服務器
		reset();

		# 如果有啟用Http服務
		if $game_configure["enable_http"]
			@util_server = UtilServer.new("pay_server")
			@util_server.start_server("0.0.0.0", 3000)
		end

	end
	
	# 已經運行的時間
	def server_running_time()
		return Time.now - @server_start_time
	end
	
	# 重啟所有服務
	def reset()
		
		self.stop()
		
		for server_con in $game_configure["services"]
			class_name = server_con["server_class"]
			con_file = server_con["configure_file"]
			server = eval("#{class_name}.new(\"#{con_file}\")")
			@services << server
		end
		
		for server in @services
			server.start
		end
		
		
	end
	
	# 停止所有服務
	def stop
		
		for server in @services
			server.stop
		end
		@services.clear
		GC.start
		
	end

	def scheduler_update(dt, times, &proc)
		server = @services.first
		if(server != nil)
			server.scheduler_update(dt, times, proc);
		end
	end
	
	# 找到一個節點
	def find_node_by_name(name)
		for server in @services
			node = server.find_node_by_name(name)
			if(node != nil)
				return node
			end
		end
		return nil
	end
	
	def warn(log)
		@logger_file.warn(log);
		print log, "\n"
	end
	
	def err(log)
		@logger_file.error(log);
		print log, "\n"
		if($@ != nil)
			for ermsg in $@
				@logger_file.error(ermsg);
				print ermsg,"\n"
			end
		end
	end
	
	def info(log)
		@logger_file.info(log)
		print log, "\n"
	end
	
	
	
end
