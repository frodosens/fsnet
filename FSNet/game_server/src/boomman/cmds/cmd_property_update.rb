
require 'cmds/boomman_pack_type.rb'
class CMDPropertyUpdate < Pack
	
	CMD_PROPERTY_UPDATE_GOLD_UPDATE = 0
	CMD_PROPERTY_UPDATE_PRESTIGE_UPDATE = 1
	CMD_PROPERTY_UPDATE_DIAMONDS_UPDATE = 2
	
	def version
		return 0;
	end
	
	class << self
		
		def create_gold_update(new_gold)
			
			os = FSOutputStream.new();
			os.write_byte(CMD_PROPERTY_UPDATE_GOLD_UPDATE);
			os.write_int32(new_gold);
			
			
			return create( 0, PACK_PROPERTY_UPDATE, os );
			
		end
		
		def create_prestige_update(new_prestige)
			
			os = FSOutputStream.new();
			os.write_byte(CMD_PROPERTY_UPDATE_PRESTIGE_UPDATE);
			os.write_int32(new_prestige);
			return create( 0, PACK_PROPERTY_UPDATE, os );
			
		end
		
		def create_diamonds_udpate(new_diamonds_update)
			
			os = FSOutputStream.new();
			os.write_byte(CMD_PROPERTY_UPDATE_DIAMONDS_UPDATE);
			os.write_int32(new_diamonds_update);
			return create( 0, PACK_PROPERTY_UPDATE, os );
		end
		
		
	end
	
	
	
end