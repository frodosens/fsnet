CREATE DATABASE `boomman` /*!40100 DEFAULT CHARACTER SET utf8 */;

use boomman
CREATE TABLE `tb_heros` (
  `serial` int(11) NOT NULL,
  `id` int(11) DEFAULT NULL,
  `name` varchar(128) DEFAULT NULL,
  `star` int(11) DEFAULT NULL,
  `quliaty` int(11) DEFAULT NULL,
  `skill` int(11) DEFAULT NULL,
  `max_hp` int(11) DEFAULT NULL,
  `hp` int(11) DEFAULT NULL,
  `boom_damage` int(11) DEFAULT NULL,
  `defe` int(11) DEFAULT NULL,
  `move_speed` decimal(4,4) DEFAULT NULL,
  `bomb_count` int(11) DEFAULT NULL,
  `bomb_rang` int(11) DEFAULT NULL,
  `improved_level` int(11) DEFAULT NULL,
  `succeed` int(11) DEFAULT NULL,
  PRIMARY KEY (`serial`),
  UNIQUE KEY `serial_UNIQUE` (`serial`)
) ENGINE=MyISAM DEFAULT CHARSET=latin1;

CREATE TABLE `tb_mail` (
  `serial` int(11) NOT NULL,
  `send_pid` int(11) DEFAULT NULL,
  `recv_pid` int(11) DEFAULT NULL,
  `unread` int(11) DEFAULT NULL,
  `title` varchar(128) DEFAULT NULL,
  `content` varchar(1024) DEFAULT NULL,
  `attachment1_id` int(11) DEFAULT NULL,
  `attachment1_count` int(11) DEFAULT NULL,
  `attachment2_id` int(11) DEFAULT NULL,
  `attachment2_count` int(11) DEFAULT NULL,
  `attachment3_id` int(11) DEFAULT NULL,
  `attachment3_count` int(11) DEFAULT NULL,
  `attachment4_id` int(11) DEFAULT NULL,
  `attachment4_count` int(11) DEFAULT NULL,
  PRIMARY KEY (`serial`),
  UNIQUE KEY `serial_UNIQUE` (`serial`)
) ENGINE=MyISAM DEFAULT CHARSET=latin1;

CREATE TABLE `tb_player` (
  `pid` int(11) NOT NULL DEFAULT '0',
  `level` int(11) DEFAULT NULL,
  `sex` int(11) DEFAULT NULL,
  `name` varchar(128) CHARACTER SET utf8 DEFAULT NULL,
  `morale` int(11) DEFAULT NULL,
  `ap` int(11) DEFAULT NULL,
  `exp` int(11) DEFAULT NULL,
  `gold` int(11) DEFAULT NULL,
  `diamonds` int(11) DEFAULT NULL,
  `prestige` int(11) DEFAULT NULL,
  `guild_id` int(11) DEFAULT NULL,
  PRIMARY KEY (`pid`),
  UNIQUE KEY `pid_UNIQUE` (`pid`)
) ENGINE=MyISAM DEFAULT CHARSET=latin1;

CREATE TABLE `tb_user` (
  `uid` int(11) NOT NULL AUTO_INCREMENT,
  `user_name` varchar(128) DEFAULT NULL,
  `user_pwd` varchar(128) DEFAULT NULL,
  `uuid` varchar(128) DEFAULT NULL,
  PRIMARY KEY (`uid`),
  UNIQUE KEY `uid_UNIQUE` (`uid`),
  UNIQUE KEY `user_name_UNIQUE` (`user_name`)
) ENGINE=MyISAM AUTO_INCREMENT=9 DEFAULT CHARSET=latin1;



