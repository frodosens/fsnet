
require 'cmds/boomman_pack_type.rb'
class CMDDBExecute < Pack
	
	def version
		return 0;
	end
	
	
	class << self
	
	
		def execute_sqls(sqls)
			
			for sql in sqls
				$game_database.execute(sql)
			end
			
		end
		
		def create_execute_sqls(sqls)
			
			os = FSOutputStream.new();
			os.write_int16(sqls.length);
			for sql in sqls
				os.write_string(sql)
			end
			
			return create( 0, PACK_TYPE_EXECUTE_SQL, os );
			
		end
		
	end
	
	
	
end