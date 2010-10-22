/*
 *  
 *  
 *  Copyright (C) 2010  Dennis Schafroth <dennis@schafroth.com>
 *  Copyright (C) 2010  Johannes Thoma <johannes.thoma@gmx.at>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "csync2.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <string.h>
#include "db_api.h"
#include "db_postgres.h"

#ifdef HAVE_POSTGRESQL_LIBPQ_FE_H
#include <postgresql/libpq-fe.h>
#endif

#if (!defined HAVE_POSTGRES)
int db_pgsql_open(const char *file, db_conn_p *conn_p)
{
	return DB_ERROR;
}
#else

/* This function parses a URL string like pgsql://[user[:passwd]@]hostname[:port]/database.
   and returns the result in the given parameters.

   If an optional keyword is not given, the value of the parameter is not changed.
*/

static int db_pgsql_parse_url(char *url, char **host, char **user, char **pass, char **database, unsigned int *port) 
{
  char *pos = strchr(url, '@'); 
  if (pos) {
    *(pos) = 0;
    *(user) = url;
    url = pos + 1;

    pos = strchr(*user, ':');
    if (pos) {
      *(pos) = 0;
      *(pass) = (pos +1);
    }
  }
  *host = url;
  pos = strchr(*host, '/');
  if (pos) {
    // Database
    (*pos) = 0;
    *database = pos+1;
  }
  pos = strchr(*host, ':');
  if (pos) {
    (*pos) = 0;
    *port = atoi(pos+1);
  }
  return DB_OK;
}

int db_pgsql_open(const char *file, db_conn_p *conn_p)
{
  PGconn *pg_conn;
  char *host, *user, *pass, *database;
  unsigned int port = 5432;  /* default postgres port */
  char *db_url = malloc(strlen(file)+1);
  char *create_database_statement;
  char *pg_conn_info;

  if (db_url == NULL)
    csync_fatal("No memory for db_url\n");

  user = "postgres";
  pass = "";
  host = "localhost";
  database = "csync2";

  strcpy(db_url, file);
  int rc = db_pgsql_parse_url(db_url, &host, &user, &pass, &database, &port);
  if (rc != DB_OK)
    return rc;

  ASPRINTF(&pg_conn_info, "host='%s' user='%s' password='%s' dbname='%s' port=%d",
	host, user, pass, database, port);

  pg_conn = PQconnectdb(pg_conn_info);
  if (pg_conn == NULL)
    csync_fatal("No memory for postgress connection handle\n");

  if (PQstatus(pg_conn) != CONNECTION_OK) {
    csync_debug(0, "Connection failed: %s", PQerrorMessage(pg_conn));
    PQfinish(pg_conn);
    return DB_ERROR;
  }

#if 0
    if (mysql_errno(db) == ER_BAD_DB_ERROR) {
      if (mysql_real_connect(db, host, user, pass, NULL, port, unix_socket, 0) != NULL) {
	ASPRINTF(&create_database_statement, "create database %s", database)

	csync_debug(2, "creating database %s\n", database);
        if (mysql_query(db, create_database_statement) != 0)
          csync_fatal("Cannot create database %s: Error: %s\n", database, mysql_error(db));
	free(create_database_statement);

	mysql_close(db);
	db = mysql_init(0);

        if (mysql_real_connect(db, host, user, pass, database, port, unix_socket, 0) == NULL)
          goto fatal;
      }
    } else
fatal:
      csync_fatal("Failed to connect to database: Error: %s\n", mysql_error(db));
  }
#endif

  db_conn_p conn = calloc(1, sizeof(*conn));

  if (conn == NULL)
    csync_fatal("No memory for conn\n");

  *conn_p = conn;
  conn->private = pg_conn;
  conn->close = db_postgres_close;
  conn->exec = db_postgres_exec;
  conn->errmsg = db_postgres_errmsg;
  conn->prepare = db_postgres_prepare;
//  conn->upgrade_to_schema = db_mysql_upgrade_to_schema;

  return DB_OK;
}


void db_postgres_close(db_conn_p conn)
{
  if (!conn)
    return;
  if (!conn->private)
    return;
  PQfinish(conn->private);
  conn->private = 0;
}

const char *db_postgres_errmsg(db_conn_p conn)
{
  if (!conn)
    return "(no connection)";
  if (!conn->private)
    return "(no private data in conn)";
  return PQerrorMessage(conn->private);
}


int db_postgres_exec(db_conn_p conn, const char *sql) 
{
  PGresult *res;

  if (!conn)
    return DB_NO_CONNECTION;

  if (!conn->private) {
    /* added error element */
    return DB_NO_CONNECTION_REAL;
  }
  res = PQexec(conn->private, sql);
  switch (PQresultStatus(res)) {
  case PGRES_COMMAND_OK:
  case PGRES_TUPLES_OK:
    return DB_OK;

  default:
    return DB_ERROR;
  }
}


int db_postgres_prepare(db_conn_p conn, const char *sql, db_stmt_p *stmt_p,
		     char **pptail)
{
  PGresult *result;
  int *row_p;

  *stmt_p = NULL;

  if (!conn)
    return DB_NO_CONNECTION;

  if (!conn->private) {
    /* added error element */
    return DB_NO_CONNECTION_REAL;
  }
  result = PQexec(conn->private, sql);

  if (result == NULL)
    csync_fatal("No memory for result\n");

  switch (PQresultStatus(result)) {
  case PGRES_COMMAND_OK:
  case PGRES_TUPLES_OK:
    break;

  default:
    csync_debug(1, "Error in PQexec: %s", PQresultErrorMessage(result));
    PQclear(result);
    return DB_ERROR;
  }

  row_p = malloc(sizeof(*row_p));
  if (row_p == NULL)
    csync_fatal("No memory for row\n");
  *row_p = -1;

  db_stmt_p stmt = malloc(sizeof(*stmt));
  if (stmt == NULL)
    csync_fatal("No memory for stmt\n");

  stmt->private = result;
  stmt->private2 = row_p;

  *stmt_p = stmt;
  stmt->get_column_text = db_postgres_stmt_get_column_text;
  stmt->get_column_blob = db_postgres_stmt_get_column_blob;
  stmt->get_column_int = db_postgres_stmt_get_column_int;
  stmt->next = db_postgres_stmt_next;
  stmt->close = db_postgres_stmt_close;
  stmt->db = conn;
  return DB_OK;
}


const void* db_postgres_stmt_get_column_blob(db_stmt_p stmt, int column)
{
  PGresult *result;
  int *row_p;

  if (!stmt || !stmt->private || !stmt->private2) {
    return 0;
  }
  result = (PGresult*)stmt->private;
  row_p = (int*)stmt->private2;

  if (*row_p >= PQntuples(result) || *row_p < 0) {
    csync_debug(1, "row index out of range (should be between 0 and %d, is %d)\n", 
                *row_p, PQntuples(result));
    return NULL;
  }
  return PQgetvalue(result, *row_p, column);
}

const char *db_postgres_stmt_get_column_text(db_stmt_p stmt, int column)
{
  PGresult *result;
  int *row_p;

  if (!stmt || !stmt->private || !stmt->private2) {
    return 0;
  }
  result = (PGresult*)stmt->private;
  row_p = (int*)stmt->private2;

  if (*row_p >= PQntuples(result) || *row_p < 0) {
    csync_debug(1, "row index out of range (should be between 0 and %d, is %d)\n", 
                *row_p, PQntuples(result));
    return NULL;
  }
  return PQgetvalue(result, *row_p, column);
}

int db_postgres_stmt_get_column_int(db_stmt_p stmt, int column)
{
  PGresult *result;
  int *row_p;

  if (!stmt || !stmt->private || !stmt->private2) {
    return 0;
  }
  result = (PGresult*)stmt->private;
  row_p = (int*)stmt->private2;

  if (*row_p >= PQntuples(result) || *row_p < 0) {
    csync_debug(1, "row index out of range (should be between 0 and %d, is %d)\n", 
                *row_p, PQntuples(result));
    return 0;
  }
  return atoi(PQgetvalue(result, *row_p, column));
}


int db_postgres_stmt_next(db_stmt_p stmt)
{
  PGresult *result;
  int *row_p;

  if (!stmt || !stmt->private || !stmt->private2) {
    return 0;
  }
  result = (PGresult*)stmt->private;
  row_p = (int*)stmt->private2;

  (*row_p)++;
  if (*row_p >= PQntuples(result))
    return DB_DONE;

  return DB_ROW;
}

int db_postgres_stmt_close(db_stmt_p stmt)
{
  PGresult *res = stmt->private;

  PQclear(res);
  free(stmt->private2);
  free(stmt);
  return DB_OK;
}


#if 0

int db_mysql_upgrade_to_schema(db_conn_p db, int version)
{
	if (version < 0)
		return DB_OK;

	if (version > 0)
		return DB_ERROR;

	csync_debug(2, "Upgrading database schema to version %d.\n", version);

	if (db_exec(db,
		"CREATE TABLE `action` ("
		"  `filename` varchar(4096) CHARACTER SET utf8 COLLATE utf8_bin DEFAULT NULL,"
		"  `command` text,"
		"  `logfile` text,"
		"  UNIQUE KEY `filename` (`filename`(326),`command`(20))"
		")"
		) != DB_OK)
		return DB_ERROR;

	if (db_exec(db,
		"CREATE TABLE `dirty` ("
		"  `filename` varchar(4096) CHARACTER SET utf8 COLLATE utf8_bin DEFAULT NULL,"
		"  `forced`   int(11)      DEFAULT NULL,"
		"  `myname`   varchar(50)  DEFAULT NULL,"
		"  `peername` varchar(50)  DEFAULT NULL,"
		"  UNIQUE KEY `filename` (`filename`(316),`peername`),"
		"  KEY `dirty_host` (`peername`(10))"
		")"
		) != DB_OK)
		return DB_ERROR;

	if (db_exec(db,
		"CREATE TABLE `file` ("
		"  `filename` varchar(4096) CHARACTER SET utf8 COLLATE utf8_bin DEFAULT NULL,"
		"  `checktxt` varchar(200) DEFAULT NULL,"
		"  UNIQUE KEY `filename` (`filename`(333))"
		")"
		) != DB_OK)
		return DB_ERROR;

	if (db_exec(db,
		"CREATE TABLE `hint` ("
		"  `filename` varchar(4096) CHARACTER SET utf8 COLLATE utf8_bin DEFAULT NULL,"
		"  `recursive` int(11)     DEFAULT NULL"
		")"
		) != DB_OK)
		return DB_ERROR;

	if (db_exec(db,
		"CREATE TABLE `x509_cert` ("
		"  `peername` varchar(50)  DEFAULT NULL,"
		"  `certdata` varchar(255) DEFAULT NULL,"
		"  UNIQUE KEY `peername` (`peername`)"
		")"
		) != DB_OK)
		return DB_ERROR;

	return DB_OK;
}


#endif /*0*/
#endif