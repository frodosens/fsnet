
#==========================================================================================
#   道具实例
#   这里的道具一般是客户端中体现的 一格道具
# 	By Frodo	2014-06-10
#==========================================================================================
class Item
	
	attr_reader :serial
	attr_reader :templete_id
	attr_reader :quality
	attr_reader :strelevel
	attr_reader :stack
	attr_reader :deleted
	attr_accessor :owner_pid
	attr_accessor :inserted

	#==========================================================================================
	# => 初始化
	#==========================================================================================
	def initialize
		@serial = 0
		@templete_id = 0
		@strelevel = 0
		@owner_pid = 0
		@stack = 0
		@deleted = 0
		@quality = 0
		@inserted = false
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
	# => 设置是否已经删除
	#==========================================================================================
	def delete=(v)
		@deleted = v ? 1 : 0
	end
	#==========================================================================================
	# => 是否已经删除
	#==========================================================================================
	def deleted?
		return @deleted == 1
	end

	#==========================================================================================
	# => 如果调用了item不存在的方法,尝试从模板中调用
	#==========================================================================================
	def method_missing(method_name, *arg, &block)
		
		if(templete != nil)
			return templete.method(method_name).call(*arg);
		end
		
	end
	
	#==========================================================================================
	# => 生成insert的sql
	#==========================================================================================
	def generate_inert_sql()
		sql = "insert into tb_items(serial, templete_id, quality, strelevel, owner_pid, stack, deleted) values( #{@serial}, #{@templete_id}, #{@quality}, #{@stack}, #{@owner_pid}, #{@stack}, #{@deleted} )"
		return sql
	end
	
	#==========================================================================================
	# => 生成update的sql
	#==========================================================================================
	def generate_update_sql()
		sql = "update tb_items set quality=#{@quality}, strelevel=#{@strelevel}, owner_pid=#{@owner_pid}, stack=#{@stack}, deleted=#{@deleted} where serial=#{@serial}"
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

	#==========================================================================================
	# => 讲道具写入输出流
	#==========================================================================================
	def write_to_stream(os)
		os.write_uint32(serial)
		os.write_string(templete.name)
		os.write_byte(templete.type)
		os.write_byte(templete.subtype)
		os.write_string(templete.icon)
		os.write_string(templete.big_icon)
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
		@templete_id   = tmp_id
		@quality 			 = self.templete.init_qualitylevel
		@strelevel    = self.templete.init_strelevel;
		@serial        = $game_database.incr("incr_item_serial");
	end

	#==========================================================================================
	# => 从模板ID创建一个道具
	#==========================================================================================
	def self.create_from_id(tmp_id)
		if(@@item_templete[tmp_id].nil?)
			raise( "create_from_id #{tmp_id} templete is nil" )
			return nil;
		end
		hero = Item.new();
		hero.init_from_templete_id(tmp_id);
		return hero;
	end
	
	
	
	
	#========================================================================
	# => 模板数据定义
	#========================================================================
	@@item_templete = {}

	def self.reload_templete()
		@@item_templete = {}
		
		CSV.load_csv("res/tmp_item.csv") do |hash|
			item_templete = ItemTemplete.create_from_hash(hash);
			@@item_templete[item_templete.id] = item_templete
		end
		
	end
end
