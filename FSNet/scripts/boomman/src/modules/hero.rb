#==========================================================================================
#   英雄卡牌实例
# 	By Frodo	2014-06-10
#==========================================================================================
class Hero
	
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
	attr_accessor :inserted 		# 是否已经入库
	attr_accessor :deleted  		# 是否已经删除
	
	def initialize
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
		@inserted 		 = false
		@deleted 			 = 0
	end
	

	#==========================================================================================
	# => 战斗力
	#==========================================================================================
	def battle_point
		return 0;
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
	# => 获取最大生命
	#==========================================================================================
	def max_hp
		return tmp_level.max_hp + tmp_stre.max_hp 
	end
	#==========================================================================================
	# => 获取生命值
	#==========================================================================================
	def attack
		return tmp_level.attack + tmp_stre.attack
	end
	#==========================================================================================
	# => 获取防御值
	#==========================================================================================
	def defence
		return tmp_level.defence + tmp_stre.defence
	end
	#==========================================================================================
	# => 获取移动速度
	#==========================================================================================
	def move_speed
		return tmp_level.move_speed + tmp_stre.move_speed
	end
	#==========================================================================================
	# => 获取可放置的炸弹数
	#==========================================================================================
	def bomb_num
		return tmp_level.bomb_num + tmp_stre.bomb_num
	end
	#==========================================================================================
	# => 获取炸弹爆炸范围
	#==========================================================================================
	def bomb_range
		return tmp_level.bomb_range + tmp_stre.bomb_range
	end
	#==========================================================================================
	# => 获取最大弹药量
	#==========================================================================================
	def bomb_max_reload
		return tmp_level.bomb_reload + tmp_stre.bomb_reload
	end

	#==========================================================================================
	# => 已被继承
	#==========================================================================================
	def inherited?
		return @inherited == 1
	end
	
	#==========================================================================================
	# => 是否已被删除
	#==========================================================================================
	def deleted?
		return @deleted == 1
	end
	def deleted=(v)
		@deleted = v ? 1 : 0
	end
	
	
	#==========================================================================================
	# => 通过模板初始化hero
	#==========================================================================================
	def init_from_templete_id(tmp_id, owner_pid)
		@templete_id   = tmp_id
		
		begin
		
			@level         = self.templete.init_level;
			@star_level    = self.templete.init_starlevel
			@quality_level = self.templete.init_qualitylevel;
			@stre_level    = self.templete.init_strelevel;
			@owner_pid     = owner_pid;
			@name					 = self.templete.name
			@serial        = $game_database.incr("incr_hero_serial");
			
			return true;
		rescue => err
			print("init_from_templete_id(#{tmp_id}, #{owner_pid})")
			print(err, err.message);
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
		os.write_uint16(defence)
		os.write_float(move_speed)
		os.write_uint16(bomb_num)
		os.write_uint16(bomb_range)
		os.write_uint16(bomb_reload);
		os.write_uint16(bomb_max_reload)
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
	# => 生成insert的sql
	#==========================================================================================
	def generate_inert_sql()
		sql = "insert into tb_heros(serial, owner_pid, level, name, templete_id, stre_level, quality_level, inherited, bomb_reload, deleted) values( #{serial}, #{@owner_pid}, #{@level}, '#{@name}', #{@templete_id}, #{@stre_level}, #{@quality_level}, #{@inherited}, #{@bomb_reload}, #{@deleted} )"
		return sql
	end
	
	#==========================================================================================
	# => 生成update的sql
	#==========================================================================================
	def generate_update_sql()
		sql = "update tb_heros set owner_pid=#{@owner_pid}, level=#{@level}, name='#{@name}', templete_id=#{@templete_id}, stre_level=#{@stre_level}, quality_level=#{@quality_level}, inherited=#{@inherited}, bomb_reload=#{@bomb_reload}, deleted=#{@deleted} where serial=#{@serial} "
		return sql
	end
	
	#==========================================================================================
	# => 生成入库的sql
	#==========================================================================================
	def generate_save_sql()
		if(@inserted)
			return generate_update_sql();
		else
			return generate_inert_sql();	
		end
	end
	
	#========================================================================
	# => 模板数据定义
	#========================================================================
	@@hero_level   = {}
	@@hero_model   = {}
	@@hero_quality = {}
	@@hero_star    = {}
	@@hero_stre    = {}
	@@heros        = {}
	@@hero_serial  = 0

	def self.reload_templete()
		
		@@hero_level = {}
		CSV.load_csv("res/tmp_hero_level.csv") do |hash|
			hero_level = HeroLevel.create_from_hash(hash);
			@@hero_level[hero_level.level] = hero_level
		end
		@@hero_model = {}
		CSV.load_csv("res/tmp_hero_model.csv") do |hash|
			hero_model = HeroModel.create_from_hash(hash);
			@@hero_model[hero_model.id] = hero_model
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
	end
end