
require 'cmds/boomman_pack_type.rb'
class CMDItemUpdate < Pack
	
	CMD_ITEM_UPDATE_GAIN = 0	# 得到一个新道具
	CMD_ITEM_UPDATE_LOSE = 1	# 失去一个道具
	CMD_ITEM_UPDATE_UPDATE = 2	# 更新一个道具
	
	
	def version
		return 0;
	end
	
	class << self
		
		def create_gain_item(*item)

			return create_item_update( items, [], [] )
			
		end
		
		def create_lose_item(*serials)

			return create_item_update( [], [], serials )
			
		end
		
		def create_update_item(*item)
			return create_item_update( [], items, [] )
		end
		
		# => new_items 新增的道具
		# => updated_items	需要更加的道具
		# => deleted_items	需要删除的道具
		def create_item_update(new_items, updated_items, deleted_items)
			
			os = FSOutputStream.new();
			os.write_uint16(new_items.length)
			for item in new_items
				item.write_to_stream(os);
			end
			
			os.write_uint16(updated_items.length)
			for item in updated_items
				os.write_uint32(item.serial)
				os.write_uint16(item.stack)
			end
			
			os.write_uint16(deleted_items.length)
			for serial in deleted_items
				os.write_uint32(item.serial)
			end
			

			return create( 0, PACK_TYPE_ITEM_UPDATE, os );
			
			
		end
		
	end
	
	
	
end