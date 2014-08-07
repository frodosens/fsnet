
require 'tcp_pack.rb'

class Pack < TCPPack
	
	# 标准数据头
	PACK_HEAD_LENGTH = (2 + 2 + 1 + 2 + 4)
  
  attr_accessor  :serial;
  attr_reader  :pack_type;
  attr_reader  :version;
  attr_reader  :input;
	
  def initialize(pack_type)
    super()
		@pack_type = pack_type;
		@version = 0;
		@serial = 0;
  end 
  
	def init_from_is( is, copy=false )
		@serial     = is.read_uint32();	# 2
    @pack_type  = is.read_int16();	# 2
    @version    = is.read_byte();		# 1
    @make_sum   = is.read_int16();  # 2
    @data_len   = is.read_int32();  # 4
		if(copy)
			data     = is.read_data(@data_len);
			nos = FSOutputStream.new();
			nos.write_uint32(@serial);
			nos.write_int16(@pack_type);
			nos.write_byte(@version);
			nos.write_int16(@make_sum);
			nos.write_int32( @data_len );
			nos.write_data( data, @data_len );
			self.write_data = nos;
			data = nil
		else
			@input       = is;
			self.read_data = @input;
		end
	end
	
  def init_from_parse_fs_pack(fs_pack)
    
    _read_data  = fs_pack.read_data;
		init_from_is( _read_data );
		
  end 
	
	def check_sum
		return 0;
	end
	
  class << self
    
    def create( serial, pack_type , os )
			pack = Pack.new(pack_type);
			pack.write_data.write_uint32( serial );
			pack.write_data.write_int16( pack.pack_type );
			pack.write_data.write_byte( pack.version );	
			pack.write_data.write_uint16( pack.check_sum ); 
			if(os.nil?)
				pack.write_data.write_int32( 0 );
			else
				pack.write_data.write_int32( os.len );
				pack.write_data.write_data( os.data, os.len );
			end
			pack.serial = serial;
			
			return pack;
    end
		
		@@_agent_serial = 1
    def generate_serial
      @@_agent_serial = (@@_agent_serial + 1)
      serial = @@_agent_serial
      return serial
    end

		def create_agent( agent_id, pack, serial=nil)

			if(serial == nil)
				serial = generate_serial()
			end
			os = FSOutputStream.new()
			os.write_int32( agent_id );
			if(pack.read_data.nil?)
				os.write_data( pack.write_data.data, pack.write_data.len );
			else
				os.write_data( pack.read_data.data, pack.read_data.len );
			end
			ret = create( serial, PACK_TYPE_AGENT, os);
			return ret
		end
		
		def create_from_agent_pack( agent_pack )
      
      pack = Pack.new(0);
			pack.init_from_is( agent_pack.input, true );
			
			return pack;
		end
		
		def create_from_is( is )
      
      pack = Pack.new(0);
			pack.init_from_is( is );
			
			return pack;
		end
		
    def parse( fs_pack );
      
      pack = Pack.new(0);
      pack.init_from_parse_fs_pack(fs_pack);
      
      return pack
      
    end
    
  end
  


end