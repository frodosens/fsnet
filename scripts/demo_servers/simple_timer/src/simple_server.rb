require "game_server.rb"


class GameServer
	PACK_TYPE_HELLO = 1
end


class SimpleServer < GameServer
	
	attr_reader :tick_task
	def on_start_complete
		super()
		

		@tick_task = scheduler_update(1.0, -1, proc{ |dt|
			p dt
		})
		
		
	end
	
	
	def cmd_hello(sender, pack)
		
		v1 = pack.input.read_byte
		v2 = pack.input.read_byte
		v3 = pack.input.read_byte
		
		os = FSOutputStream.new
		os.write_byte(v1)
		os.write_byte(v2)
		os.write_byte(v3)
		
		ret = Pack.create( pack.serial,  PACK_TYPE_HELLO, os )
		sender.send_pack(ret)
		
	end
		
end
