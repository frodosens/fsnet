

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
	
	

end
