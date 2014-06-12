
require 'cmds/boomman_pack_type.rb'
class CMDHeroUpdate < Pack
	
	CMD_HERO_UPDATE_GAIN = 0	# 得到一个新英雄
	CMD_HERO_UPDATE_LOSE = 1	# 失去一个英雄
	CMD_HERO_UPDATE_UPDATE = 2	# 更新一个英雄
	
	
	def version
		return 0;
	end
	
	class << self
		
		def create_gain_hero(hero)
			
			os = FSOutputStream.new();
			os.write_byte(CMD_HERO_UPDATE_GAIN);
			hero.write_to_stream(os);
			return create( 0, PACK_TYPE_HERO_UPDATE, os );
			
		end
		
		def create_lose_hero(serial)
			
			os = FSOutputStream.new();
			os.write_byte(CMD_HERO_UPDATE_LOSE);
			os.write_int32(serial);
			return create( 0, PACK_TYPE_HERO_UPDATE, os );
			
		end
		
		def create_update_hero(hero)
			
			os = FSOutputStream.new();
			os.write_byte(CMD_HERO_UPDATE_UPDATE);
			hero.write_to_stream(os);
			return create( 0, PACK_TYPE_HERO_UPDATE, os );
		end
		
		
	end
	
	
	
end