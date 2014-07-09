
class BaseModule
	
	attr_reader   :templete_id
	attr_reader   :serial
	attr_reader 	:user_data
	def initialize
		@deleted 			 = 0
		@templete_id   = 0
		@serial        = 0
		@user_data		 = {}
	end
	
	def templete
		return nil
	end
	
	
	#==========================================================================================
	# => 用户数据
	#==========================================================================================
	def user_data
		@user_data ||= {}
		return @user_data 
	end
	
	
	#==========================================================================================
	# => 从模板ID初始化
	#==========================================================================================
	def init_from_templete_id(templete_id)
		@templete_id = templete_id
		if( self.templete != nil )
			@serial = $game_database.incr("incr_#{self.class.name}_serial");
			return true
		else
			return false
		end
	end
	
	#==========================================================================================
	# => 从模板ID实例化
	#==========================================================================================
	def self.create_from_templete_id(templete_id)
		ret = new()
		if(ret.init_from_templete_id(templete_id))
			return ret
		else
			return nil
		end
	end
	
	#==========================================================================================
	# => 找到方法
	#==========================================================================================
	def method(method_name)
		
		ret = nil
		begin 
			ret = super(method_name)
		rescue => err
			if(self.templete != nil)
				ret = self.templete.method(method_name)
			end
		end
		return ret
	end
	
	#==========================================================================================
	# => 如果调用了自己不存在的方法,尝试从模板中调用
	#==========================================================================================
	def method_missing(method_name, *arg, &block)
		
		if(self.templete != nil)
			return templete.method(method_name).call(*arg);
		end
		
		raise("#{self.class} call #{method_name} error")
		return nil
	end
	
	
	#==========================================================================================
	# => 设置数值时候的回调
	#==========================================================================================
	def on_setting_variable(key, value)
		
		return false
	end
	


	#==========================================================================================
	# => 从hash初始化
	#==========================================================================================
	def init_from_hash(hash)
		for key, value in hash
			parm_class = String
		
			if(key != nil)
				begin
					parm_class = self.method(key).call().class	
					if(parm_class == NilClass)
						parm_class = String
					end
				rescue => err
					parm_class = String
				end
			end
			# 缺省默认值
			if(value == nil)
				if(parm_class == String)
					value = ""
				elsif (parm_class == Array)
					value = "[]"
				elsif (parm_class == Hash)
					value = "{}"
				else
					value = 0
				end
			end
			if((parm_class == Hash or parm_class == Array) and value.class == String)
				value = value.gsub("|", ",")
			end
			# 
			if(key != nil)
				if(!on_setting_variable(key, value))
					if(parm_class == String || parm_class == Array || parm_class == Hash)
						if(value == "")
							self.instance_eval( "@#{key}='#{value}'" );
						else
							self.instance_eval( "@#{key}=YAML.load('#{value}')" );
						end
						
					else
						if(!on_setting_variable(key, value))
							self.instance_eval( "@#{key}=#{value}" );
						end
						
					end
				end
			end
		end
		return self
	end
	

	#==========================================================================================
	# => 深度拷贝
	#==========================================================================================
	def deep_clone
		return YAML.load(self.to_yaml)
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
	# => 生成入库的sql
	#==========================================================================================
	def generate_save_sql()
		return ""
	end 
	
	
	#==========================================================================================
	# => 获取这个数据体的缓存cache
	#==========================================================================================
	def cache_key
		raise("no impl the cache_key")
		return ""
	end
	
	#==========================================================================================
	# => 缓存到redis
	#==========================================================================================
	def cache

		$game_database.set( self.cache_key(), self.to_yaml )
		
	end
	class << self
		
		#==========================================================================================
		# => 通过befor_key得出缓存key
		#==========================================================================================
		def generate_cache_key(befor_key)
			raise("no impl the befor_key")
		end
		
		#==========================================================================================
		# => 通过redis创建一个module
		#==========================================================================================
		def create_from_redis(befor_key)
			key = generate_cache_key(befor_key)
			yaml = $game_database.get(key);
			if(yaml != nil)
				return YAML.load(yaml);
			end
		end
		
	end
	
end