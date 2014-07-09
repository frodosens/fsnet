

class GameDatabase

	attr_reader :sql_connect;
	attr_reader :cache;
	
	def initialize(parm = nil)
		
		init_caches();
		init_db_connect(parm)
		
	end
	
	def init_caches()
		
		@cache = CacheGroupManager.new($game_configure["redis_configure"])
	
	end
	
	def init_db_connect(parm)
		
		if( parm == nil )
			
			@sql_connect      = Mysql2::Client.new(
			:host => $game_configure["mysql_configure"]["host"],
			:username => $game_configure["mysql_configure"]["username"],
			:password => $game_configure["mysql_configure"]["password"],
			:database => $game_configure["mysql_configure"]["database"],
			:port => $game_configure["mysql_configure"]["port"]);
			
			@sql_parm = {}
			@sql_parm[:host] = $game_configure["mysql_configure"]["host"]
			@sql_parm[:username] = $game_configure["mysql_configure"]["username"]
			@sql_parm[:password] = $game_configure["mysql_configure"]["password"]
			@sql_parm[:database] = $game_configure["mysql_configure"]["database"]
			@sql_parm[:port] = $game_configure["mysql_configure"]["port"]
			
		else
			@sql_connect = Mysql2::Client.new(parm);
			@sql_parm = parm
		end
		
	end

	def query(sql)
		result = nil
		begin
			result = @sql_connect.query(sql);
		rescue Mysql2::Error => err
			# 主键重复
			if (err.error_number == 1062 || err.error_number == 1169)
				return nil
			end
			# 语法错误
			if (err.error_number == 1149)
				$game.err( " sql syntax error : #{sql} ");
				return nil
			end
			
			# 网络错误,重新连接
			if(err.error_number == 1023 || 
				err.error_number == 1079 || 
				err.error_number == 1080 || 
				err.error_number == 1081 || 
				err.error_number == 1129 ||
				err.error_number == 1043 ||  # 无效连接
				err.error_number == 2003)  # 无效连接
				$game.err(err.message);
				@sql_connect = Mysql2::Client.new(@sql_parm);
			end
			
		end
		return result;
	end
	
	def execute(sql)
		query(sql);
		return @sql_connect.last_id
	end
	
	def try_remoot_execute(sqls)
		
		if(sqls.class == String)
			sqls = [sqls]
		end
		
		db_node = $game.find_node_by_name("db_server")
		
		if(db_node.nil?)
			for sql in sqls
				execute(sql);
			end
			
		else
			
			pack = CMDDBExecute.create_execute_sqls( sqls )
			
			db_node.send_pack(pack)
			
		end
		
		
	end
	
	# 持久化缓存
	def cache2storage(key, value)
		if(value.class == String)
			value = SafeSql.conver(value)
		end
		sql = "replace into tb_redis(r_key, r_value) values('#{key}', '#{value}')"
		try_remoot_execute(sql)
	end
	
	# 从DB读取cache
	def storage2cache(key)
		sql = "select r_key,r_value from tb_redis where r_key='#{key}' limit 0,1"
		result = query(sql)
		value = nil
		if(result and result.size > 0)
			result.each do |row|
				key = row["r_key"]
				value = row["r_value"]
				set(key, value, false)
			end
		end
		return value
	end
	
	
	# 获取一个值
	def get(key)
		value = @cache.get(key)
		if(value.nil?)
			value = storage2cache(key)
		end
		return value
	end
	
	# 设置一个值
	def set(key, value, storage=true)
		if(storage)
			cache2storage(key, value)
		end
		return @cache.set(key, value)
	end
	
	# 获取自增值
	def incr(key)
		value = @cache.incr(key);
		cache2storage(key, value)
		return value
	end
	
	
end
