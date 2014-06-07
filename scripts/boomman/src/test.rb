require 'redis.rb'

require 'mysql2'

p Encoding::ASCII_8BIT.name

@sql_connect      = Mysql2::Client.new(
:host => "10.10.1.51",
:username => "root",
:password => "123456",
:database => "boomman",
:port => 3306);

@sql_connect.query("insert into tb_player( pid, level, sex, name, morale, ap, exp, gold, diamonds, prestige, guild_id ) 
																		values( #{10000}, 0, -1, '', 0, 0, 0, 0, 0, 0, 0 ) ");