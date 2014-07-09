
require 'modules/basemode.rb'

#==========================================================================================
#   英雄卡牌实例
# 	By Frodo	2014-06-10
#==========================================================================================
class Hero < BaseModule
	
	
	attr_reader :serial					# 序列号
	attr_reader :templete_id		# 模板ID
	attr_reader :level					# 等级
	attr_reader :name						# 自定义名字
	attr_reader :star_level			# 星级
	attr_reader :quality_level	# 品质等级
	attr_reader :stre_level			# 强化等级
	attr_reader :inherited			# 是否已被继承
	attr_reader :bomb_reload 		# 弹药量
	attr_reader :owner_pid			# 持有者的PID
	
	def initialize
		super()
		@serial        = 0
		@owner_pid		 = 0
		@templete_id   = 0
		@level         = 0
		@name          = ""
		@star_level    = 0
		@quality_level = 0
		@stre_level    = 0
		@inherited     = 0
		@bomb_reload   = 0
	end
	
		
	#==========================================================================================
	# => 持有者Player
	#==========================================================================================
	def owner_player
		return $game_players[owner_pid]
	end	
	#==========================================================================================
	# => 战斗力
	#==========================================================================================
	def battle_point
		return @level + @star_level;
	end
	#==========================================================================================
	# => 获取模板
	#==========================================================================================
	def templete
		return @@heros[@templete_id];
	end
	#==========================================================================================
	# => 获取等级模板
	#==========================================================================================
	def tmp_level
		return @@hero_level[@level];
	end
	#==========================================================================================
	# => 获取星级模板
	#==========================================================================================
	def tmp_star
		return @@hero_star[@star_level];
	end
	#==========================================================================================
	# => 获取强化模板
	#==========================================================================================
	def tmp_stre
		return @@hero_stre[@stre_level];
	end
	#==========================================================================================
	# => 获取品质模板
	#==========================================================================================
	def tmp_quality
		return @@hero_quality[@quality_level];
	end
	#==========================================================================================
	# => 炸彈對自己的弱化率
	#==========================================================================================
	def boom_damage_weaken
		return 0
	end
	#==========================================================================================
	# => 获取最大生命
	#==========================================================================================
	def max_hp
		return tmp_level.max_hp + tmp_stre.max_hp  * (1 + tmp_quality.value) * (1 + tmp_star.value)
	end
	#==========================================================================================
	# => 获取生命值
	#==========================================================================================
	def attack
		return (tmp_level.attack + tmp_stre.attack) * (1 + tmp_quality.value) * (1 + tmp_star.value)
	end
	#==========================================================================================
	# => 获取防御值
	#==========================================================================================
	def defence
		return (tmp_level.defence + tmp_stre.defence) * (1 + tmp_quality.value) * (1 + tmp_star.value)
	end
	#==========================================================================================
	# => 获取移动速度
	#==========================================================================================
	def move_speed
		return AttributeTemplete.templete.base_movespeed + 
					(tmp_level.move_speed + tmp_stre.move_speed) * 
					(1 + tmp_quality.value) * (1 + tmp_star.value)
	end
	#==========================================================================================
	# => 获取可放置的炸弹数
	#==========================================================================================
	def bomb_num
		return AttributeTemplete.templete.base_boomcount + 
				(tmp_level.bomb_num + tmp_stre.bomb_num) * 
				(1 + tmp_quality.value) * (1 + tmp_star.value)
	end
	#==========================================================================================
	# => 获取炸弹爆炸范围
	#==========================================================================================
	def bomb_range
		return AttributeTemplete.templete.base_boomrange + 
				(tmp_level.bomb_range + tmp_stre.bomb_range) * 
				(1 + tmp_quality.value) * (1 + tmp_star.value)
	end
	#==========================================================================================
	# => 炸弹回复时间 
	#==========================================================================================
	def bomb_reload
		return AttributeTemplete.templete.base_boomreload + 				# 基础值
					(tmp_level.bomb_reload + tmp_stre.bomb_reload) * 			# 等级值+强化值
					(1 + tmp_quality.value) * (1 + tmp_star.value)				# 修正值
	end
	

	#==========================================================================================
	# => 減少技能CD時間
	#==========================================================================================
	def skill_cdtime
		return 0
	end

	#==========================================================================================
	# => 已被继承
	#==========================================================================================
	def inherited?
		return @inherited == 1
	end
	
	#==========================================================================================
	# => 通过模板初始化hero
	#==========================================================================================
	def init_from_templete_id(tmp_id, owner_pid)
		super(tmp_id)
		begin
		
			@level         = self.templete.init_level;
			@star_level    = self.templete.init_starlevel
			@quality_level = self.templete.init_qualitylevel;
			@stre_level    = self.templete.init_strelevel;
			@owner_pid     = owner_pid;
			@name					 = self.templete.name
			
			return true;
		rescue => err
			$game.err("init_from_templete_id(#{tmp_id}, #{owner_pid})")
			return false
		end
		
	end
	

	#==========================================================================================
	# => 将英雄写入输出流中
	#==========================================================================================
	def write_to_stream(os)
		os.write_uint32(serial);
		os.write_uint16(level);
		os.write_string(name);
		os.write_byte(star_level)
		os.write_byte(quality_level)
		os.write_byte(stre_level)
		os.write_byte(inherited)
		os.write_uint16(max_hp)
		os.write_uint16(attack)
		os.write_float(defence.to_f)
		os.write_float(move_speed)
		os.write_uint16(bomb_num)
		os.write_uint16(bomb_range)
		os.write_float(bomb_reload.to_f)
		os.write_uint16(3)		# 炸弹爆炸时间
		os.write_uint32(self.model)
		os.write_byte(max_starlevel)
		os.write_uint32(battle_point)
		os.write_string(icon)
		os.write_string(big_icon)
	end


	#==========================================================================================
	# => 获取模型ID
	#==========================================================================================
	def model
		return model_id_list[0]
	end

	#==========================================================================================
	# => 通过模板ID创建一个新的hero
	#==========================================================================================
	def self.create_from_id(tmp_id, owner_pid)
		hero = Hero.new();
		if(hero.init_from_templete_id(tmp_id, owner_pid))
			return hero;
		end
		return nil
	end
	
	#==========================================================================================
	# => 生成入库的sql
	#==========================================================================================
	def generate_save_sql()
		sql = "replace into tb_heros(serial, owner_pid, level, name, templete_id, stre_level, quality_level, inherited, bomb_reload, user_data, deleted) values( #{serial}, #{@owner_pid}, #{@level}, '#{@name}', #{@templete_id}, #{@stre_level}, #{@quality_level}, #{@inherited}, #{@bomb_reload}, '#{user_data.to_yaml}', #{@deleted} )"
		return sql
	end
	
	
	#========================================================================
	# => 模板数据定义
	#========================================================================
	@@hero_level   = {}
	@@hero_quality = {}
	@@hero_star    = {}
	@@hero_stre    = {}
	@@heros        = {}
	@@hero_serial  = 0

	def self.star_templete
		return @@hero_star
	end
	def self.quality_templete
		return @@hero_quality
	end
	
	def self.hero_level_templete
		return @@hero_level
	end

	def self.reload_templete()
		
		@@hero_level = {}
		CSV.load_csv("res/tmp_hero_level.csv") do |hash|
			hero_level = HeroLevel.create_from_hash(hash);
			@@hero_level[hero_level.level] = hero_level
		end
		@@hero_quality = {}
		CSV.load_csv("res/tmp_hero_qualitylevel.csv") do |hash|
			hero_quality = HeroQualityLevel.create_from_hash(hash);
			@@hero_quality[hero_quality.quality] = hero_quality
		end
		@@hero_star = {}
		CSV.load_csv("res/tmp_hero_starlevel.csv") do |hash|
			hero_star = HeroStarLevel.create_from_hash(hash);
			@@hero_star[hero_star.level] = hero_star
		end
		@@hero_stre = {}
		CSV.load_csv("res/tmp_hero_strelevel.csv") do |hash|
			hero_stre = HeroStreLevel.create_from_hash(hash);
			@@hero_stre[hero_stre.level] = hero_stre
		end
		@@heros = {}
		CSV.load_csv("res/tmp_hero.csv") do |row|
			hero = HeroTemplete.create_from_hash(row);
			@@heros[hero.id] = hero;
		end
		AttributeTemplete.reload_templete()
	end
end