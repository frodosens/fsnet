
class GameHomeSystem < GameSystemBase

	# 家园怪物属性模板
	class HomeMonsterAttribureTemplete < ::Monster
		
		attr_reader :id
		attr_reader :battle_point_begin
		attr_reader :battle_point_end
		attr_reader :max_hp
		attr_reader :attack
		attr_reader :defence
		attr_reader :movespeed
		attr_reader :script
		def initialize
			
			@id = 0
			@battle_point_begin = 0
			@battle_point_end = 0
			@max_hp = 0
			@attack = 0
			@defence = 0
			@movespeed = 0.0
			@script = ""
			
		end
		
	end

	# 家园怪物
	class HomeMonsterTemplete < ::Monster
		
		attr_reader :describe
		attr_reader :cost
		
		def initialize
			super
			@cost = 0
			@describe = ""
		end
		
		def write_to_stream(os)

			os.write_int32(@id)
			os.write_string(@name)
			os.write_byte(@starlevel)
			os.write_byte(@qualitylevel)
			os.write_uint16(@cost)
			os.write_string(@describe)
			os.write_uint32(model)
			
			
		end
	
	end

	class HomeMonster < BaseModule
		
		attr_reader :count						# 持有数量
		attr_reader :deployment_count	# 已经上阵的数量
		
		def initialize
			super
			@deployment_count = 0
			@count = 0
		end
		
		# 上阵
		def deploymente
			if(@deployment_count + 1 <= @count)
				@deployment_count += 1
				return true
			end
			return false
		end
		
		# 取消上阵
		def undeploymente
			if(@deployment_count - 1 >= 0)
				@deployment_count -= 1
				return true
			end
			return false
		end
		
		# 得到1个
		def gain
			@count += 1
		end
		
		# 失去1个
		def lose
			@count -= 1
		end
		
		# 获取模板
		def templete
			return @@home_monster[@templete_id]
		end
		
		# 写信息入流
		def write_info_to_stream(os)
			
			@count ||= 0
			@deployment_count ||= 0
			
			os.write_uint32(self.serial)
			templete.write_to_stream(os);
			os.write_uint16(@count)
			os.write_uint16(@deployment_count)
			
		end
		
		
		# 写战斗信息入流
		def write_to_stream(os)
			
			size_width = 1
			size_height = 1
			
			os.write_int32(id)
			os.write_string(self.name)
			os.write_int32(max_hp)
			os.write_float(movespeed.to_f)
			os.write_float(defence);
			os.write_byte(size_width)
			os.write_byte(size_height)
			os.write_int32(attack)
			os.write_string("")
			os.write_int32(self.model)
			
			
			
		end
		
		def to_pve_monster(battle_point)

			attri = HomeMonster.find_attr_by_battle_point(battle_point)
			attack = attri.attack
			defence = attri.defence
			move_speed = attri.movespeed
			max_hp = attri.max_hp
			model_id_list = self.model_id_list
			id = self.id
			name = self.name
			
			monster = Monster.new
			monster.instance_eval() { 
				@id = id
				@name = name
				@attack = attack
				@defence = defence
				@movespeed = movespeed
				@hp_limit = max_hp
				@ai_script = ""
				@model_id_list = model_id_list
			}
			
			return monster
			
		end
		
		
		
		#==========================================================================================
		# => 从模板ID初始化
		#==========================================================================================
		def init_from_templete_id(templete_id)
			ret = super(templete_id)
			if(ret)
				@count = 1
			end
			return ret
		end
		
		
		class << self
			@@home_monster = {}
			@@home_monster_attr = {}
			
			def templete_table
				return @@home_monster;
			end
			
			# reload 资源
			def reload_templete
				@@home_monster = {}
				@@home_monster_attr = {}
				CSV.load_csv("res/tmp_monster_home.csv") do |hash|
					monster = HomeMonsterTemplete.new()
					@@home_monster[hash["id"].to_i] = monster.init_from_hash(hash);
				end
				CSV.load_csv("res/tmp_monster_home_attr.csv") do |hash|
					attribure = HomeMonsterAttribureTemplete.new()
					@@home_monster_attr[hash["id"].to_i] = attribure.init_from_hash(hash);
				end
			end
			
			# 通过战斗力找到一个属性
			def find_attr_by_battle_point(bp)
				
				for key, value in @@home_monster_attr
					if(bp >= value.battle_point_begin and bp <= value.battle_point_end)
						return value
					end
				end
				
				return nil
				
			end
				
			def find_home_monster_by(mid)
				return @@home_monster[mid];
			end
		end
	
	end
	
end