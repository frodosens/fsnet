class Hero
	

	# 英雄基础模板
	class HeroTemplete
		attr_reader :id								# 模板ID
		attr_reader :name							# 模板名字
		attr_reader :type							# 模板类型
		attr_reader :model_id_list		# 模型ID组, 每个星级对于不同
		attr_reader :init_level				# 初始等级
		attr_reader :max_level				# 最大等级
		attr_reader :init_starlevel		# 初始星级
		attr_reader :max_starlevel		# 最大星级
		attr_reader :init_qualitylevel# 初始品质
		attr_reader :max_qualitylevel	# 最大品质
		attr_reader :init_strelevel		# 初始强化等级
		attr_reader :max_strelevel		# 最大强化等级
		attr_reader :tmp_skill_id			# 模板技能	
		attr_reader :tmp_equip_id_1		# 模板装备 帽子ID
		attr_reader :tmp_equip_id_2		# 模板装备 衣服ID
		attr_reader :tmp_equip_id_3		# 模板装备 鞋子ID
		attr_reader :tmp_equip_id_4		# 模板装备 首饰ID
		attr_reader :tmp_equip_id_5		# 模板装备 武器ID
		attr_reader :bomb_exp_time		# 炸弹爆炸时间
		attr_reader :icon							# ICON
		attr_reader :big_icon					# BIG ICON
	
		def initialize()
			@id = 0
			@name = ""
			@type = 0
			@model_id_list = []
			@init_level = 0
			@max_level = 0
			@init_starlevel = 0
			@max_starlevel = 0
			@init_qualitylevel = 0
			@max_qualitylevel = 0
			@init_strelevel = 0
			@max_strelevel = 0
			@tmp_skill_id = 0
			@tmp_hero_equip_id_1 = 0
			@tmp_hero_equip_id_2 = 0
			@tmp_hero_equip_id_3 = 0
			@tmp_hero_equip_id_4 = 0
			@tmp_hero_equip_id_5 = 0
			@bomb_exp_time = 2
			@icon = ""
			@big_icon = ""
		end


		def init_from_hash(hash)
			@id = hash["id"].to_i
			@name = hash["name"].to_s
			@type = hash["type"].to_i
			@model_id_list = YAML.load(hash["model_id_list"].gsub("|", ","))
			@init_level = hash["init_level"].to_i
			@max_level = hash["max_level"].to_i
			@init_starlevel = hash["init_starlevel"].to_i
			@max_starlevel = hash["max_starlevel"].to_i
			@init_qualitylevel = hash["init_qualitylevel"].to_i
			@max_qualitylevel = hash["max_qualitylevel"].to_i
			@init_strelevel = hash["init_strelevel"].to_i
			@max_strelevel = hash["max_strelevel"].to_i
			@tmp_skill_id = hash["tmp_skill_id"].to_i
			@tmp_hero_equip_id_1 = hash["tmp_hero_equip_id_1"].to_i
			@tmp_hero_equip_id_2 = hash["tmp_hero_equip_id_2"].to_i
			@tmp_hero_equip_id_3 = hash["tmp_hero_equip_id_3"].to_i
			@tmp_hero_equip_id_4 = hash["tmp_hero_equip_id_4"].to_i
			@tmp_hero_equip_id_5 = hash["tmp_hero_equip_id_5"].to_i
			@icon = hash["icon"]
			@big_icon = hash["bigicon"].to_s
		end

		def self.create_from_hash(hash)

			tmp = new();
			tmp.init_from_hash(hash);
			return tmp;

		end

	end
	
	
end