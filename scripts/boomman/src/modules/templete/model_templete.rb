
# 模型模板
class ModelTemplete
	
	class BombModelTemplete
		
		attr_reader :id
		attr_reader :name						# 描述名字
		attr_reader :arm_bone_name	# 炸弹骨骼名字
		attr_reader :bomb_scale			# 拉伸?
		attr_reader :png_filename		# 换装PNG
		attr_reader :sound_id				# 音乐ID
		attr_reader :bomb_burn_flash# 燃烧动画(FLASH)
		attr_reader :bomb_burn_ani	# 燃烧动画
		attr_reader :bomb_fire_flash# 火焰动画(FLASH)
		attr_reader :bomb_fire_ani	# 火焰动画
		def initialize
			@id = 0
			@name = ""
			@arm_bone_name = ""
			@bomb_scale = 1.0
			@png_filename = ""
			@sound_id = 0
			@bomb_burn_flash = ""
			@bomb_burn_ani = ""
			@bomb_fire_flash = ""
			@bomb_fire_ani = ""
		end
		def init_from_hash(hash)

			@id = hash["id"].to_i
			@name = hash["name"]
			@arm_bone_name = hash["tmp_res_flash_name_bombbone"];
			@bomb_scale = hash["bomb_scale"].to_f;
			@png_filename = hash["png_filename_bomb"];
			@sound_id = hash["mp3_filename_soundeffect"].to_i
			@bomb_burn_flash = hash["tmp_res_flash_name_burn"]
			@bomb_burn_ani = hash["ani_filename_burn"]
			@bomb_fire_flash = hash["tmp_res_flash_name_fire"]
			@bomb_fire_ani = hash["ani_filename_fire"]

		end
		
	
		class << self
		
			@@bomb_models = {}
			def reload_templete()
				
				@@bomb_models = {}
				CSV.load_csv("res/tmp_model_bomb.csv") do |hash|
					model = new()
					model.init_from_hash(hash)
					@@bomb_models[model.id] = model
				end
				
			end
		
			def find_model_by_id(mid)
				return @@bomb_models[mid]
			end
		
		end
		
		
	end
	
	attr_reader :id
	attr_reader :name
	attr_reader :arm_bone_name
	attr_reader :bomb_model_id

	def initialize
		@id = 0
		@name = ""
		@arm_bone_name = ""
		@bomb_model_id = 0
	end
	
	def write_to_stream(os)
		
	end

	def init_from_hash(hash)
		
		@id = hash["id"].to_i
		@name = hash["name"]
		@arm_bone_name = hash["tmp_res_flash_name_objbone"];
		@bomb_model_id = hash["bomb_model_id"].to_i;
		
	end

	def self.create_from_hash(hash)
		model = HeroModel.new()
		model.init_from_hash(hash);
		return model
	end
	
	class << self
		
		@@models = {}
		def reload_templete()
			@@models = {}
			CSV.load_csv("res/tmp_model.csv") do |hash|
				model = new()
				model.init_from_hash(hash)
				@@models[model.id] = model
			end
			
			BombModelTemplete.reload_templete()
			
		end
		
		def find_model_by_id(mid)
			return @@models[mid]
		end
		
	end


end


