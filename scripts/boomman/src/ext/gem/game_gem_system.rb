
require 'ext/game_system.rb'



class GameManagerResoucre
	
	alias :old_gem_reload_items :reload_items
	
	def reload_items
		
		result = old_gem_reload_items();
		
		result << GameGemSystem::GemMix
		result << GameGemSystem::GemEffect
		
		return result
	end
	
end


class GameGemSystem < GameSystemBase
	
	attr_reader :configure					# 宝石系统配置
	
	def start(server)
		super(server)
		
		
		configure_file = File.open(File.dirname(__FILE__) + "/gem.yaml");
		@configure = YAML.load(configure_file)
		configure_file.close();

	end
	
	def stop
		super()
		
	end
	
end