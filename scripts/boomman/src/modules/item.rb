

class Item
	
	attr_reader :serial
	attr_reader :id
	attr_reader :name
	attr_reader :desc
	attr_reader :icon
	attr_reader :big_icon
	attr_reader :stack
	attr_reader :max_stack
	
	def initialize
		@serial = 0
		@id = 0
		@name = ""
		@desc = ""
		@icon = ""
		@big_icon = ""
		@stack = 0
		@max_stack = 0;
	end
	

end
