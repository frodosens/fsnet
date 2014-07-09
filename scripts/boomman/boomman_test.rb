# encoding: ASCII-8BIT
require 'csv'
require "rexml/document"  
include REXML  

$: << File.dirname(__FILE__) + "/src"

Dir.chdir(File.dirname(__FILE__))
Dir[File.dirname(__FILE__) + '/src/manager/*.rb'].each {|file| require file } 
Dir[File.dirname(__FILE__) + '/src/modules/basemode.rb'].each {|file| require file } 
Dir[File.dirname(__FILE__) + '/src/modules/*.rb'].each {|file| require file } 
Dir[File.dirname(__FILE__) + '/src/modules/templete/*.rb'].each {|file| require file } 
Dir[File.dirname(__FILE__) + '/src/utils/*.rb'].each {|file| require file }
Dir[File.dirname(__FILE__) + '/src/ext/*.rb'].each {|file| require file }
# Dir[File.dirname(__FILE__) + '/src/ext/*/*.rb'].each {|file| require file }

Hero.reload_templete();
Item.reload_templete();
Player.reload_templete

require 'digest/md5'


t1 = Time.now

for i in 0...100000
	Math.sqrt(9)
end

t2 = Time.now

p t2 - t1

# print "bbbb".to_yaml
# print 1.0.to_yaml
# Map.create_from_file(0, "res/Maps/Map1-hd.tmx")
# Map.reload_templete
