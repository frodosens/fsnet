


require 'cmds/boomman_pack_type.rb'
class CMDEnterMap < Pack
	
	class << self
		
		def execute(request_serial, sender, server, map_id, hero_serial)
			
			player = sender.player
			player.pve_state = PVEState.new()
			player.pve_state.hero_serial = hero_serial
			player.pve_state.make_pve_data(map_id)
			
			pack = create(player.pve_state, request_serial)
			sender.send_pack(pack);
			
		end
		
	
		def create( pve_state , request_serial=0)
			os = FSOutputStream.new();
			
			map = Map.find_map_by_id(pve_state.mapid)
			
			if(map == nil)
				os.write_byte(GameCMDS::FAIL)
			else
				os.write_byte(GameCMDS::SUCCESS)
				
				pve_state.write_to_stream(os)
				
			end
			
			pack = Pack.create( request_serial, PACK_TYPE_ENTER_MAP, os )
			
			return pack
		end
		
		
	end
	
	
	
	
end