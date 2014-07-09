
# Player扩展
class Player
	
	attr_reader :home		# 家园
	
	attr_reader :home_battle_hero_serial
	attr_reader :pve_battle_hero_serial
	
	alias :old_init :init
	alias :old_on_login :on_login
	alias :old_on_logout :on_logout
	alias :old_init_from_hash :init_from_hash
	alias :old_user_data :user_data
	alias :old_save :save
	alias :old_cache :cache
	alias :old_initialze :initialize
	alias :old_clear_daily_data :clear_daily_data
	
	def initialize()
		old_initialze();
		@home = nil
		@pve_battle_hero_serial = 0
		@home_battle_hero_serial = 0
	end
	
	
	def init_home
		@home = $game_homes.load_home(self.pid)
	end
	
	def init(*args)
		old_init(*args)
		if(@home.nil?)
			self.init_home();
		end
	end
	
	def on_login()
		old_on_login()
		self.init_home();
	end
	
	def clear_daily_data
		old_clear_daily_data()
		
		# 清理Hero的所有出战次数
		for hero in @heros.values
			hero.clean_battle_count
		end
		
	end
	
	# 
	def home_battle_hero_serial=(v)
		@home_battle_hero_serial = v
	end
	
	def home_battle_hero_serial
		@home_battle_hero_serial ||= 0
		return @home_battle_hero_serial;
	end
	
	# 
	def pve_battle_hero_serial=(v)
		@pve_battle_hero_serial = v
	end
	
	def pve_battle_hero_serial
		@pve_battle_hero_serial ||= 0
		return @pve_battle_hero_serial
	end
	
	def cache()
		old_cache()
		if(@home != nil)
			@home.cache()
		end
	end
	
	def save()
		old_save()
		if(@home != nil)
			@home.save()
		end
	end
	
	#==========================================================================================
	# => 用户数据
	#==========================================================================================
	def user_data
		base = old_user_data()
		return base;
	end
	
end