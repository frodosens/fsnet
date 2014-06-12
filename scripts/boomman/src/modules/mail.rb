

class Mail
	
	
	attr_reader :serial
	attr_reader :send_pid
	attr_reader :recv_pid
	attr_reader :unread
	attr_reader :title
	attr_reader :content
	attr_reader :attachment1_id
	attr_reader :attachment1_count
	attr_reader :attachment2_id
	attr_reader :attachment2_count
	attr_reader :attachment3_id
	attr_reader :attachment3_count
	attr_reader :attachment4_id
	attr_reader :attachment4_count
	
	def initialize
		
		@serial = 0
		@send_pid = 0
		@recv_pid = 0
		@unread = 1
		@title = ""
		@content = ""
		@attachment1_id = 0
		@attachment1_count = 0
		@attachment2_id = 0
		@attachment2_count = 0
		@attachment3_id = 0
		@attachment3_count = 0
		@attachment4_id = 0
		@attachment4_count = 0
		
	end
	
end