
CREATE DATABASE peregrine CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

CREATE USER 'peregrineuser'@'localhost' IDENTIFIED BY 'peregrinepass';

GRANT ALL PRIVILEGES ON peregrine.* TO 'peregrineuser'@'localhost';

CREATE TABLE colors (
	id char(64),
	name char(25)
);

INSERT INTO colors VALUES(sha2('red', 256), 'red');
INSERT INTO colors VALUES(sha2('green', 256), 'green');
INSERT INTO colors VALUES(sha2('blue', 256), 'blue');

DELIMITER ;;
CREATE PROCEDURE `sp_colors`()
BEGIN 
	SELECT * FROM colors;
END;;
DELIMITER ;
