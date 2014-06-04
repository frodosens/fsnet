

class Player
	
	attr_reader :pid
	attr_reader :name
	attr_reader :level
	attr_reader :exp
	attr_reader :gold
	attr_reader :diamonds
	attr_reader :prestige
	attr_reader :heros
	attr_reader :items
	
	def initialize()
		
	end
	
	def find_item_by_id(item_id)
		for it in items
			
			if(it.id == item_id)
				return it;
			end
			
		end
		return nil;
	end
	
	def gain_item(item_id, item_count)
		item = find_item_by_id(item_id);
		# 如果有现有的道具可叠加
		if(item != NULL and item.stack < item.max_stack)
			item.stack += 1;
		end
	end
	
	def lose_item(item_id, item_count)
		
	end
	
	def gain_exp(exp)
		
	end
	
	def level_up()
	end
	
	def gain_gold(val)
		@gold += val
		@gold = ( @gold < 0 ? 0 : @gold);
		@gold = ( @gold > $game_configure["max_gold"] ? $game_configure["max_gold"] : @gold);
		return @gold
	end
	
	def gain_diamonds(val)
		@diamonds += val
		@diamonds = ( @diamonds < 0 ? 0 : @diamonds);
		@diamonds = ( @diamonds > $game_configure["max_diamonds"] ? $game_configure["max_diamonds"] : @diamonds);
		return @diamonds
	end
	
	def gain_prestige(val)
		@prestige += val
		@prestige = ( @prestige < 0 ? 0 : @prestige);
		@prestige = ( @prestige > $game_configure["max_prestige"] ? $game_configure["max_prestige"] : @prestige);
		return @prestige
	end
	
	
	def lose_gold(val)
		return gain_gold(-val);
	end
	def lsoe_diamonds(val)
		return gain_diamonds(-val);
	end
	def lose_prestige(val)
		return gain_prestige(-val);
	end
	
end



