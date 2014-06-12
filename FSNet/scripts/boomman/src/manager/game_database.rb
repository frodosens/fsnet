

class GameDatabase

	attr_reader :sql_connect;
	attr_reader :redis;
	
	def initialize(parm = nil)
		
		
		@redis = Redis.new( :host=> $game_configure["redis_configure"]["host"], :port => $game_configure["redis_configure"]["port"]);
		
		if( parm == nil )
			@sql_connect      = Mysql2::Client.new(
			:host => $game_configure["mysql_configure"]["host"],
			:username => $game_configure["mysql_configure"]["username"],
			:password => $game_configure["mysql_configure"]["password"],
			:database => $game_configure["mysql_configure"]["database"],
			:port => $game_configure["mysql_configure"]["port"]);
			
		else
			@sql_connect      = Mysql2::Client.new(parm);
		end
		
	end

	def query(sql)
		return @sql_connect.query(sql);
	end
	
	def execute(sql)
		query(sql);
		return @sql_connect.last_id
	end
	
	def try_remoot_execute(server, sqls)
		
		if(server.nil?)
			for sql in sqls
				execute(sql);
			end
			
		else
			
			db_node = server.find_node_by_name("db_server")
			
			if(db_node.nil?)
				try_remoot_execute(nil, sql)
				return
			end
			
			pack = CMDDBExecute.create_execute_sqls( sqls )
			
			db_node.send_pack(pack)
			
		end
		
		
	end
	
	def incr(key)
		return @redis.incr(key);
	end
end
