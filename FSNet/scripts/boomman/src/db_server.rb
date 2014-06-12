require 'cmds/boomman_pack_type.rb'
require "game_server.rb"

class DBServer < GameServer
	
		
		
	def execute_sql(sender, pack)
		
		sql_count = pack.input.read_int16
		sqls = []
		for i in 0...sql_count
			sqls << pack.input.read_string()
		end
		
		CMDDBExecute.execute_sqls(sqls)
		
	end
	
end
