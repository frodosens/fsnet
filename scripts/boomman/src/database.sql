CREATE DATABASE `boomman`; /*!40100 DEFAULT CHARACTER SET utf8 */;
CREATE TABLE `tb_heros` (
  `serial` int(11) NOT NULL,
  `owner_pid` int(11) DEFAULT NULL,
  `level` int(11) DEFAULT NULL,
  `name` varchar(128) CHARACTER SET utf8 DEFAULT NULL,
  `templete_id` int(11) DEFAULT NULL,
  `stre_level` int(11) DEFAULT NULL,
  `quality_level` int(11) DEFAULT NULL,
  `inherited` int(11) DEFAULT NULL,
  `bomb_reload` int(11) DEFAULT NULL,
  `user_data` varchar(2048) DEFAULT NULL,
  `deleted` int(11) DEFAULT NULL,
  PRIMARY KEY (`serial`),
  UNIQUE KEY `serial_UNIQUE` (`serial`),
  KEY `owner_index` (`owner_pid`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8;

CREATE TABLE `tb_homes` (
  `pid` int(11) NOT NULL,
  `rank` int(11) DEFAULT NULL,
  `cur_home_serial` int(11) DEFAULT NULL,
  `maps` varchar(2048) DEFAULT NULL,
  `monsters` varchar(2048) DEFAULT NULL,
  `guards` varchar(2048) DEFAULT NULL,
  `match_history` varchar(2048) DEFAULT NULL,
  PRIMARY KEY (`pid`),
  UNIQUE KEY `pid_UNIQUE` (`pid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

CREATE TABLE `tb_items` (
  `serial` int(11) NOT NULL,
  `templete_id` int(11) DEFAULT NULL,
  `quality` int(11) DEFAULT NULL,
  `strelevel` int(11) DEFAULT NULL,
  `owner_pid` int(11) DEFAULT NULL,
  `stack` int(11) DEFAULT NULL,
  `deleted` int(11) DEFAULT NULL,
  PRIMARY KEY (`serial`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

CREATE TABLE `tb_mail` (
  `serial` int(11) NOT NULL,
  `send_pid` int(11) DEFAULT NULL,
  `send_name` varchar(45) CHARACTER SET utf8 DEFAULT NULL,
  `send_date` int(11) DEFAULT NULL,
  `recv_pid` int(11) DEFAULT NULL,
  `unread` int(11) DEFAULT NULL,
  `title` varchar(128) CHARACTER SET utf8 DEFAULT NULL,
  `content` varchar(1024) CHARACTER SET utf8 DEFAULT NULL,
  `attachment1_id` int(11) DEFAULT NULL,
  `attachment1_count` int(11) DEFAULT NULL,
  `attachment2_id` int(11) DEFAULT NULL,
  `attachment2_count` int(11) DEFAULT NULL,
  `attachment3_id` int(11) DEFAULT NULL,
  `attachment3_count` int(11) DEFAULT NULL,
  `attachment4_id` int(11) DEFAULT NULL,
  `attachment4_count` int(11) DEFAULT NULL,
  `deleted` int(11) DEFAULT NULL,
  PRIMARY KEY (`serial`),
  UNIQUE KEY `serial_UNIQUE` (`serial`),
  KEY `recv_index` (`recv_pid`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8;

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
  `user_data` varchar(2048) DEFAULT NULL,
  PRIMARY KEY (`pid`),
  UNIQUE KEY `pid_UNIQUE` (`pid`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8;

CREATE TABLE `tb_redis` (
  `r_key` varchar(45) NOT NULL,
  `r_value` varchar(10240) DEFAULT NULL,
  PRIMARY KEY (`r_key`),
  UNIQUE KEY `key_UNIQUE` (`r_key`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

CREATE TABLE `tb_user` (
  `uid` int(11) NOT NULL AUTO_INCREMENT,
  `pid` varchar(45) DEFAULT NULL,
  `user_name` varchar(128) DEFAULT NULL,
  `user_pwd` varchar(128) DEFAULT NULL,
  `uuid` varchar(128) DEFAULT NULL,
  PRIMARY KEY (`uid`),
  UNIQUE KEY `uid_UNIQUE` (`uid`),
  UNIQUE KEY `user_name_UNIQUE` (`user_name`)
) ENGINE=MyISAM AUTO_INCREMENT=39 DEFAULT CHARSET=utf8;
