require 'modules/basemode.rb'
#==========================================================================================
#   道具实例
#   这里的道具一般是客户端中体现的 一格道具
# 	By Frodo	2014-06-10
#==========================================================================================
class Item < BaseModule
	
	# 物品類型
	ITEM_TYPE_EQUIP            = 1	# 裝備
	ITEM_TYPE_CONSUME          = 2	# 消耗品
	ITEM_TYPE_POTION           = 3	# 藥水
	ITEM_TYPE_MONSTER_FRAGMENT = 4	# 守衛碎片
	ITEM_TYPE_GEMS             = 5	# 寶石
	ITEM_TYPE_BOX              = 6	# 寶箱

	# 裝備子類型
	ITEM_TYPE_EQUIP_WEAPON     = 1	# 武器
	ITEM_TYPE_EQUIP_DECORATION = 2	# 首飾
	ITEM_TYPE_EQUIP_HELMET     = 3	# 頭盔
	ITEM_TYPE_EQUIP_CLOTHES    = 4	# 衣服
	ITEM_TYPE_EQUIP_SHOES      = 5	# 鞋子

	
	
	attr_reader :serial
	attr_reader :templete_id
	attr_reader :quality
	attr_reader :strelevel
	attr_reader :stack
	attr_accessor :owner_pid
	
	alias :id :templete_id

	#==========================================================================================
	# => 初始化
	#==========================================================================================
	def initialize
		super()
		@serial = 0
		@templete_id = 0
		@strelevel = 0
		@owner_pid = 0
		@stack = 0
		@quality = 0
	end
	

	#==========================================================================================
	# => 设置堆叠数
	#==========================================================================================
	def stack=(v)
		@stack = v
		if(@stack <= 0)
			self.delete = true
		end
	end
	
	#==========================================================================================
	# => 生成入库的sql
	#==========================================================================================
	def generate_save_sql()
		sql = "replace into tb_items(serial, templete_id, quality, strelevel, owner_pid, stack, deleted) values( #{@serial}, #{@templete_id}, #{@quality}, #{@stack}, #{@owner_pid}, #{@stack}, #{@deleted} )"
		return sql
	end
	
	#==========================================================================================
	# => 讲道具写入输出流
	#==========================================================================================
	def write_to_stream(os)
		os.write_uint32(serial)
		os.write_uint32(templete.id)
		os.write_string(templete.name)
		os.write_byte(templete.type)
		os.write_byte(templete.subtype)
		os.write_string(templete.icon)
		os.write_string(templete.big_icon)
		os.write_string(templete.describe)
		os.write_uint16(stack)
		os.write_uint16(max_stack)
		os.write_byte(quality)
		os.write_uint16(max_hp)
		os.write_uint16(attack)
		os.write_uint16(defence)
		os.write_float(move_speed)
		os.write_uint16(bomb_num)
		os.write_uint16(bomb_range)
		os.write_uint16(bomb_reload)
	end

	#==========================================================================================
	# => 获取模板
	#==========================================================================================
	def templete
		return @@item_templete[@templete_id];
	end

	#==========================================================================================
	# => 从模板ID初始化一个道具
	#==========================================================================================
	def init_from_templete_id(tmp_id)
		super(tmp_id)
		begin
			@quality 			 = self.templete.init_qualitylevel
			@strelevel    = self.templete.init_strelevel;
			@serial        = $game_database.incr("incr_item_serial");
			return true;
		rescue => err
			raise(" 未定义模板物品 #{tmp_id}")
			return false
		end	
	end

	#==========================================================================================
	# => 从模板ID创建一个道具
	#==========================================================================================
	def self.create_from_id(tmp_id)
		item = Item.new();
		if(item.init_from_templete_id(tmp_id))
			return item;
		end
		return nil
	end
	
	
	
	
	#========================================================================
	# => 模板数据定义
	#========================================================================
	@@item_templete = {}
	
	def self.find_templete(id)
		return @@item_templete[id]
	end

	def self.reload_templete()
		@@item_templete = {}
		
		CSV.load_csv("res/tmp_item.csv") do |hash|
			item_templete = ItemTemplete.create_from_hash(hash);
			@@item_templete[item_templete.id] = item_templete
		end
		
	end
end
