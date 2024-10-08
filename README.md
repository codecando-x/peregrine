# peregrine

![peregrine logo](https://github.com/codecando-x/peregrine/blob/main/peregrine.png)

Call a MySQL Stored Procedure and cache in Redis via an Apache Module.

Ubuntu Requirements:

		sudo apt install apache2 apache2-dev libhiredis-dev redis mysql-server libmysqlclient-dev

Install https://github.com/akheron/jansson


Compile:

		sudo apxs -l hiredis -l jansson -I /usr/include/mysql -i -a -c peregrine.c `mysql_config --cflags --libs`
		
Add the following to a virtual host config:

		<Location "/peregrine">
			SetHandler peregrine-handler
		</Location>
		
Add these configs to the apache2.conf file

		peregrineValidationRegex "^[a-zA-Z0-9-]+$"
		peregrineParamName "sp"
		peregrineCacheSocket "/var/run/redis/redis-server.sock"
		peregrineDatabaseUsername "peregrineuser"
		peregrineDatabasePassword "peregrinepass"
		peregrineDatabaseName "peregrine"
		peregrineDatabaseSocket "/var/run/mysqld/mysqld.sock"

Enable the config, enable the mod, restart and test:
		
		sudo a2enmod peregrine
		sudo a2ensite peregrine; //if you created a completely new virtual host config
		sudo service apache2 restart;
		curl -X GET localhost/peregrine/fly?sp=colors


