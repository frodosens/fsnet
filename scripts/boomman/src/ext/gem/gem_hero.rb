
# Gems Hero扩展
class Hero
	
	alias :gem_boom_damage_weaken :boom_damage_weaken
	alias :gem_max_hp							:max_hp
	alias :gem_attack							:attack
	alias :gem_defence						:defence
	alias :gem_move_speed					:move_speed
	alias :gem_bomb_reload				:bomb_reload
	alias :gem_skill_cdtime				:skill_cdtime

	
	#==========================================================================================
	# => 炸彈對自己的弱化率
	#==========================================================================================
	def boom_damage_weaken
		v = gem_boom_damage_weaken();
		gems = owner_player.gems
		v *= (1 + gems.boom_damage_weaken_rate)
		v += gems.boom_damage_weaken
		return v
	end
	#==========================================================================================
	# => 获取最大生命
	#==========================================================================================
	def max_hp
		v = gem_max_hp();
		gems = owner_player.gems
		v *= (1 + gems.max_hp_rate)
		v += gems.max_hp
		return v
	end
	#==========================================================================================
	# => 获取生命值
	#==========================================================================================
	def attack
		v = gem_attack();
		gems = owner_player.gems
		v *= (1 + gems.attack_rate)
		v += gems.attack
		return v
	end
	#==========================================================================================
	# => 获取防御值
	#==========================================================================================
	def defence
		v = gem_defence();
		gems = owner_player.gems
		v *= (1 + gems.defence_rate)
		v += gems.defence
		return v
	end
	#==========================================================================================
	# => 获取移动速度
	#==========================================================================================
	def move_speed
		v = gem_move_speed();
		gems = owner_player.gems
		v *= (1 + gems.move_speed_rate)
		v += gems.move_speed
		return v
	end
	#==========================================================================================
	# => 炸弹回复时间 
	#==========================================================================================
	def bomb_reload
		v = gem_bomb_reload();
		gems = owner_player.gems
		v *= (1 + gems.boom_reload_rate)
		v += gems.boom_reload
		return v
	end

	#==========================================================================================
	# => 技能CD時間
	#==========================================================================================
	def skill_cdtime
		v = gem_skill_cdtime();
		gems = owner_player.gems
		v *= (1 + gems.skill_cdtime_rate)
		v += gems.skill_cdtime
		return v
	end
	
	
end