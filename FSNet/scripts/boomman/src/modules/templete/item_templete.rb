
class Item
	
	class ItemTemplete
		
		attr_reader :id
		attr_reader :name
		attr_reader :type
		attr_reader :subtype
		attr_reader :max_stack
		attr_reader :init_qualitylevel
		attr_reader :init_strelevel
		attr_reader :max_strelevel
		attr_reader :max_hp
		attr_reader :attack
		attr_reader :defence
		attr_reader :move_speed
		attr_reader :bomb_num
		attr_reader :bomb_range
		attr_reader :bomb_reload
		attr_reader :cdtime
		attr_reader :use_effect
		attr_reader :tmp_buff_id
		attr_reader :sale_price
		attr_reader :icon
		attr_reader :big_icon
		attr_reader :describe
		
		def initialize()
			@id = 0
			@name = ""
			@type = 0
			@subtype = 0
			@max_stack = 0
			@init_qualitylevel = 0
			@init_strelevel = 0
			@max_strelevel = 0
			@max_hp = 0
			@attack = 0
			@defence = 0
			@move_speed = 0.0
			@bomb_num = 0
			@bomb_range = 0
			@bomb_reload = 0
			@cdtime = 0
			@use_effect = nil
			@tmp_buff_id = 0
			@sale_price = 0
			@icon = ""
			@big_icon = ""
			@describe = ""
		end
			
		def init_from_hash(hash)
			
			@id = hash["id"].to_i
			@name = hash["name"]
			@type = hash["type"].to_i
			@subtype = hash["subtype"].to_i
			@max_stack = hash["max_pile"].to_i
			@init_qualitylevel = hash["inital_qualitylevel"].to_i
			@init_strelevel = hash["inital_strlevel"].to_i
			@max_strelevel = hash["max_strelevel"].to_i
			@max_hp = hash["hp_limit_add"].to_i
			@attack = hash["attack_add"].to_i
			@defence = hash["defence_add"].to_i
			@move_speed = hash["movespeed_mut"].to_f
			@bomb_num = hash["maxbombnum_mut"].to_i
			@bomb_range = hash["maxbombrange_mut"].to_i
			@bomb_reload = hash["maxbombreload_mut"].to_i
			@cdtime = hash["cdtime"]
			@use_effect = hash["use_effect"]
			@tmp_buff_id = hash["tmp_buff_id"].to_i
			@sale_price = hash["sale_price"].to_i
			@icon = hash["icon"]
			@big_icon = hash["bigicon"]
			@describe = hash["describe"]
			
		end
		
		def self.create_from_hash(hash)
			tmp = ItemTemplete.new()
			tmp.init_from_hash(hash);
			return tmp;
		end
			
	end
end