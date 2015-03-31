
class FSPack

  def byte_order 
    return 0; 
  end
  
  def write_only?
    return read_data.nil?
  end
    
  def read_only?
    return write_data.nil?
  end


end

class TCPPack < FSPack

  def initialize()
    super()
		#self.write_data = FSOutputStream.new
  end
  

end