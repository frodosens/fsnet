require "game_server.rb"


class GameServer
	PACK_TYPE_GET_TIME = 2
end


class TimeServer < GameServer
	
	
	attr_reader :db_tick_task
	def on_start_complete
		super()
	end
	
	
	def cmd_time(sender, pack)
		
		now = Time.now.to_i
		
		os = FSOutputStream.new
		os.write_uint32(now)
		ret = Pack.create( pack.serial,  PACK_TYPE_GET_TIME, os )
		sender.send_pack(ret)
		
	end
		
end
