
# Hero扩展
class Hero
	
	attr_reader :battle_count
	
	alias :old_write_to_stream :write_to_stream
	alias :old_initialze :initialize
	
	def initialize()
		old_initialze();
		@battle_count = 0
	end
	
	# 今日已上阵次数
	def battle_count
		@battle_count ||= 0
		return @battle_count
	end
	
	# 最大可战斗次数
	def hero_battle_count
		return $game_homes.configure["hero_battle_count"]
	end
	
	# 可以参战
	def can_be_battle?
		return battle_count < hero_battle_count
	end
	
	# 添加战斗次数
	def add_battle_count
		return @battle_count += 1
	end
	
	def clean_battle_count
		return @battle_count = 0
	end
	
	#==========================================================================================
	# => 将英雄写入输出流中
	#==========================================================================================
	def write_to_stream(os)
		old_write_to_stream(os);
		os.write_uint16(battle_count)
		os.write_uint16(hero_battle_count)
	end
	
end