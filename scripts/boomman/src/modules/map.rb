
require 'modules/basemode.rb'

#==========================================================================================
#   地图对象
# 	By Frodo	2014-06-13
#==========================================================================================
class MapObject < BaseModule
	
	attr_accessor :mid
	attr_accessor :x
	attr_accessor :y
	attr_accessor :type
	attr_accessor :name
	
	def initialize
		@mid = 0
		@x = 0
		@y = 0
		@type = ""
		@name = ""
	end
	
	def monster?
		return @type == "MONSTER"
	end
	
	def monster
		return Monster.find_pve_monster_by(@mid)
	end
	
	def write_to_stream(os)
		os.write_int32(@mid)
		monster.write_to_stream(os)
	end
	
end

#==========================================================================================
#   地图实例
# 	By Frodo	2014-06-13
#==========================================================================================
class Map 
	
	attr_reader :map_id
	attr_reader :map_xml
	attr_reader :map_width
	attr_reader :map_height
	attr_reader :tilewidth
	attr_reader :tileheight
	attr_reader :objects
	attr_reader :gold
	attr_reader :exp
	attr_reader :items
	
	def initialize(map_id)
		@map_width = 0
		@map_height = 0
		@map_id = map_id;
		@objects = []
	end
	
	#==========================================================================================
	# => 怪物数量
	#==========================================================================================
	def monster_count
		count = 0;
		@objects.each{ |m| count += (m.monster? and m.monster != nil) ? 1 : 0 }
		return count;
	end
	
	#==========================================================================================
	# => 从文件初始化
	#==========================================================================================
	def init_from_file(file_name)
		
		parser = XML::Parser.file(file_name, :encoding => XML::Encoding::UTF_8, :options => XML::Parser::Options::NOENT)   
	  doc = parser.parse   
		map_node = doc.find_first("/map")
		@map_width = map_node["width"].to_i
		@map_height = map_node["height"].to_i
		@map_xml = doc.to_s
		@tilewidth = map_node["tilewidth"].to_i
		@tileheight = map_node["tileheight"].to_i
		
		
		map_node.find("layer").each do |layer|
			layer_data= layer.find_first("data").content
		end
		
		object_group = map_node.find("objectgroup/object")
		object_group.each do |object|
			

			object_x = object["x"].to_i / @tilewidth
			object_y = (@map_height - 1) - (object["y"].to_i / @tileheight)
			
			map_object = MapObject.new();
			map_object.x = object_x
			map_object.y = object_y
			map_object.name = object["name"]
			map_object.type = object["type"]
			
			object.find("properties/property").each do |pro|
				if(pro["name"] == "mid")
						map_object.mid = pro["value"].to_i
				end
			end
			
			
			@objects << map_object;
			
		end

		doc = nil
		object_group = nil
		map_node = nil
		
		return true
	end
	
	
	#==========================================================================================
	# => 写入流
	#==========================================================================================
	def write_to_stream(os)
		
		os.write_string(@map_xml)
		
	end
	
	
	#==========================================================================================
	# => MD5
	#==========================================================================================
	def md5
		return MD5Util.md5(@map_xml)
	end
	
	
	class << self
		
		@@maps = {}
		def reload_templete
			@@maps = {}
			for file_name in Dir["res/Maps/*"]
				if file_name.match /[Mm][Aa][Pp]([0-9]+)-hd.tmx$/
					@@maps[$1.to_i] = Map.create_from_file($1, file_name)
					if(block_given?)
						yield file_name
					end
				end
			end
		end
		
		def find_map_by_id(map_id)
			return @@maps[map_id];
		end
		
		def create_from_file(id, file)
			map = Map.new(id)
			if(map.init_from_file(file))
				return map
			end
			return nil
		end
		
	end
	
	
end