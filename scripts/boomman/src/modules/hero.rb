

class Hero
	
	attr_reader :serial					# 序列号
	attr_reader :id							# ID
	attr_reader :name						# 名字
	attr_reader :star						# 星
	attr_reader :quliaty				# 品质
	attr_reader :skill					# 技能ID
	attr_reader :max_hp					# MAXHP
	attr_reader :hp							# HP
	attr_reader :bomb_damage		# 炸弹伤害
	attr_reader :defe						# 防御
	attr_reader :move_speed			# 移动速度
	attr_reader :bomb_count			# 炸弹数量
	attr_reader :bomb_rang			# 炸弹范围
	attr_reader :equip1
	attr_reader :equip2
	attr_reader :equip3
	attr_reader :equip4	
	attr_reader :equip5					
	attr_reader :improved_level	# 强化等级
	attr_reader :succeed				# 是否已经被继承
	
	def initialize
		@serial = 0
		@id = 0
		@name = ""
		@star = 0
		@quliaty = 0
		@skill = 0
		@max_hp = 0
		@hp = 0
		@bomb_damage = 0
		@defe = 0
		@move_speed = 0.0
		@bomb_count = 0
		@bomb_rang = 0
		@improved_level = 0
		@succeed = false
	end
	
	def battle_point
		return 0;
	end
	
	
end