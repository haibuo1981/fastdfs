/**
* Copyright (C) 2008 Happy Fish / YuQing
*
* FastDFS may be copied only under the terms of the GNU General
* Public License V3, which may be found in the FastDFS source kit.
* Please visit the FastDFS Home Page http://www.csource.org/ for more detail.
**/

//tracker_service.c

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include "fdfs_define.h"
#include "fdfs_base64.h"
#include "logger.h"
#include "fdfs_global.h"
#include "sockopt.h"
#include "shared_func.h"
#include "tracker_types.h"
#include "tracker_global.h"
#include "tracker_mem.h"
#include "tracker_proto.h"
#include "tracker_service.h"

static pthread_mutex_t tracker_thread_lock;
static pthread_mutex_t lb_thread_lock;

int g_tracker_thread_count = 0;

int tracker_service_init()
{
	int result;
	if ((result=init_pthread_lock(&tracker_thread_lock)) != 0)
	{
		return result;
	}

	if ((result=init_pthread_lock(&lb_thread_lock)) != 0)
	{
		return result;
	}

	return 0;
}

int tracker_service_destroy()
{
	pthread_mutex_destroy(&tracker_thread_lock);
	pthread_mutex_destroy(&lb_thread_lock);

	return 0;
}

static int tracker_check_and_sync(TrackerClientInfo *pClientInfo, \
			const int status)
{
	TrackerHeader resp;
	FDFSStorageDetail **ppServer;
	FDFSStorageDetail **ppEnd;
	FDFSStorageBrief briefServers[FDFS_MAX_SERVERS_EACH_GROUP];
	FDFSStorageBrief *pDestServer;
	int out_len;
	int result;

	memset(&resp, 0, sizeof(resp));
	resp.cmd = TRACKER_PROTO_CMD_STORAGE_RESP;
	resp.status = status;

	if (status != 0 || pClientInfo->pGroup == NULL ||
		pClientInfo->pGroup->version == pClientInfo->pStorage->version)
	{
		if ((result=tcpsenddata_nb(pClientInfo->sock, \
			&resp, sizeof(resp), g_network_timeout)) != 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"client ip: %s, send data fail, " \
				"errno: %d, error info: %s", \
				__LINE__, pClientInfo->ip_addr, \
				result, strerror(result));
			return result;
		}

		return status;
	}

	//printf("sync %d servers\n", pClientInfo->pGroup->count);

	pDestServer = briefServers;
	ppEnd = pClientInfo->pGroup->sorted_servers + \
			pClientInfo->pGroup->count;
	for (ppServer=pClientInfo->pGroup->sorted_servers; \
		ppServer<ppEnd; ppServer++)
	{
		pDestServer->status = (*ppServer)->status;
		memcpy(pDestServer->ip_addr, (*ppServer)->ip_addr, \
			IP_ADDRESS_SIZE);
		pDestServer++;
	}

	out_len = sizeof(FDFSStorageBrief) * pClientInfo->pGroup->count;
	long2buff(out_len, resp.pkg_len);
	if ((result=tcpsenddata_nb(pClientInfo->sock, \
		&resp, sizeof(resp), g_network_timeout)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, send data fail, " \
			"errno: %d, error info: %s", \
			__LINE__, pClientInfo->ip_addr, \
			result, strerror(result));
		return result;
	}

	if ((result=tcpsenddata_nb(pClientInfo->sock, \
		briefServers, out_len, g_network_timeout)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, send data fail, " \
			"errno: %d, error info: %s", \
			__LINE__, pClientInfo->ip_addr, \
			result, strerror(result));
		return result;
	}

	pClientInfo->pStorage->version = pClientInfo->pGroup->version;
	return status;
}

static void tracker_check_dirty(TrackerClientInfo *pClientInfo)
{
	bool bInserted;
	if (pClientInfo->pGroup != NULL && pClientInfo->pGroup->dirty)
	{
		tracker_mem_pthread_lock();
		if (--(*pClientInfo->pGroup->ref_count) == 0)
		{
			free(pClientInfo->pGroup->ref_count);
			free(pClientInfo->pAllocedGroups);
		}
		tracker_mem_pthread_unlock();

		tracker_mem_add_group(pClientInfo, true, &bInserted);
	}

	if (pClientInfo->pStorage != NULL && pClientInfo->pStorage->dirty)
	{
		tracker_mem_pthread_lock();
		if (--(*pClientInfo->pStorage->ref_count) == 0)
		{
			free(pClientInfo->pStorage->ref_count);
			free(pClientInfo->pAllocedStorages);
		}
		tracker_mem_pthread_unlock();

		tracker_mem_add_storage(pClientInfo, true, &bInserted);
	}
}

static int tracker_deal_storage_replica_chg(TrackerClientInfo *pClientInfo, \
				const int64_t nInPackLen)
{
	TrackerHeader resp;
	int server_count;
	FDFSStorageBrief briefServers[FDFS_MAX_SERVERS_EACH_GROUP];
	int result;

	memset(&resp, 0, sizeof(resp));
	do
	{
		if ((nInPackLen <= 0) || \
			(nInPackLen % sizeof(FDFSStorageBrief) != 0))
		{
			logError("file: "__FILE__", line: %d, " \
				"cmd=%d, client ip addr: %s, " \
				"package size "INT64_PRINTF_FORMAT" " \
				"is not correct", \
				__LINE__, \
				TRACKER_PROTO_CMD_STORAGE_REPLICA_CHG, \
				pClientInfo->ip_addr, nInPackLen);
			resp.status = EINVAL;
			break;
		}

		server_count = nInPackLen / sizeof(FDFSStorageBrief);
		if (server_count > FDFS_MAX_SERVERS_EACH_GROUP)
		{
			logError("file: "__FILE__", line: %d, " \
				"client ip addr: %s, return storage count: %d" \
				" exceed max: %d", __LINE__, \
				pClientInfo->ip_addr, server_count, \
				FDFS_MAX_SERVERS_EACH_GROUP);
			resp.status = EINVAL;
			break;
		}

		if ((resp.status=tcprecvdata_nb(pClientInfo->sock, briefServers, \
			nInPackLen, g_network_timeout)) != 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"client ip addr: %s, recv data fail, " \
				"errno: %d, error info: %s.", \
				__LINE__, pClientInfo->ip_addr, \
				resp.status, strerror(resp.status));
			break;
		}

		resp.status = tracker_mem_sync_storages(pClientInfo, \
				briefServers, server_count);
	} while (0);

	resp.cmd = TRACKER_PROTO_CMD_STORAGE_RESP;
	if ((result=tcpsenddata_nb(pClientInfo->sock, \
		&resp, sizeof(resp), g_network_timeout)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, send data fail, " \
			"errno: %d, error info: %s", \
			__LINE__, pClientInfo->ip_addr, \
			result, strerror(result));
		return result;
	}

	return resp.status;
}

static int tracker_deal_storage_join(TrackerClientInfo *pClientInfo, \
				const int64_t nInPackLen)
{
	TrackerStorageJoinBody body;
	int store_path_count;
	int subdir_count_per_path;
	int upload_priority;
	int status;
	time_t up_time;

	do
	{
	if (nInPackLen != sizeof(body))
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd: %d, client ip: %s, package size "INT64_PRINTF_FORMAT" " \
			"is not correct, expect length: %d.", \
			__LINE__, TRACKER_PROTO_CMD_STORAGE_JOIN, \
			pClientInfo->ip_addr, nInPackLen, sizeof(body));
		status = EINVAL;
		break;
	}

	if ((status=tcprecvdata_nb(pClientInfo->sock, &body, \
		nInPackLen, g_network_timeout)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, recv data fail, " \
			"errno: %d, error info: %s", \
			__LINE__, pClientInfo->ip_addr, \
			status, strerror(status));
		break;
	}

	memcpy(pClientInfo->group_name, body.group_name, FDFS_GROUP_NAME_MAX_LEN);
	pClientInfo->group_name[FDFS_GROUP_NAME_MAX_LEN] = '\0';
	if ((status=fdfs_validate_group_name(pClientInfo->group_name)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, invalid group_name: %s", \
			__LINE__, pClientInfo->ip_addr, \
			pClientInfo->group_name);
		break;
	}

	pClientInfo->storage_port = (int)buff2long(body.storage_port);
	if (pClientInfo->storage_port <= 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, invalid port: %d", \
			__LINE__, pClientInfo->ip_addr, \
			pClientInfo->storage_port);
		status = EINVAL;
		break;
	}

	pClientInfo->storage_http_port = (int)buff2long(body.storage_http_port);
	if (pClientInfo->storage_http_port < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, invalid http port: %d", \
			__LINE__, pClientInfo->ip_addr, \
			pClientInfo->storage_http_port);
		status = EINVAL;
		break;
	}

	store_path_count = (int)buff2long(body.store_path_count);
	if (store_path_count <= 0 || store_path_count > 256)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, invalid store_path_count: %d", \
			__LINE__, pClientInfo->ip_addr, store_path_count);
		status = EINVAL;
		break;
	}

	subdir_count_per_path = (int)buff2long(body.subdir_count_per_path);
	if (subdir_count_per_path <= 0 || subdir_count_per_path > 256)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, invalid subdir_count_per_path: %d", \
			__LINE__, pClientInfo->ip_addr, subdir_count_per_path);
		status = EINVAL;
		break;
	}

	upload_priority = (int)buff2long(body.upload_priority);
	up_time = (time_t)buff2long(body.up_time);

	status = tracker_mem_add_group_and_storage(pClientInfo, \
			store_path_count, subdir_count_per_path, \
			upload_priority, up_time, true, body.init_flag);
	} while (0);

	return tracker_check_and_sync(pClientInfo, status);
}

static int tracker_deal_server_delete_storage(TrackerClientInfo *pClientInfo, \
				const int64_t nInPackLen)
{
	TrackerHeader resp;
	char in_buff[FDFS_GROUP_NAME_MAX_LEN + IP_ADDRESS_SIZE];
	char group_name[FDFS_GROUP_NAME_MAX_LEN + 1];
	char *pIpAddr;
	FDFSGroupInfo *pGroup;
	int result;

	memset(&resp, 0, sizeof(resp));
	do
	{
		if (nInPackLen <= FDFS_GROUP_NAME_MAX_LEN)
		{
			logError("file: "__FILE__", line: %d, " \
				"cmd=%d, client ip: %s, package size "INT64_PRINTF_FORMAT" " \
				"is not correct, " \
				"expect length > %d", \
				__LINE__, \
				TRACKER_PROTO_CMD_SERVER_DELETE_STORAGE, \
				pClientInfo->ip_addr,  \
				nInPackLen, FDFS_GROUP_NAME_MAX_LEN);
			resp.status = EINVAL;
			break;
		}

		if (nInPackLen >= sizeof(in_buff))
		{
			logError("file: "__FILE__", line: %d, " \
				"cmd=%d, client ip: %s, package size "INT64_PRINTF_FORMAT" " \
				"is too large, " \
				"expect length should < %d", \
				__LINE__, \
				TRACKER_PROTO_CMD_SERVER_DELETE_STORAGE, \
				pClientInfo->ip_addr, nInPackLen, \
				sizeof(in_buff));
			resp.status = EINVAL;
			break;
		}

		if ((resp.status=tcprecvdata_nb(pClientInfo->sock, in_buff, \
			nInPackLen, g_network_timeout)) != 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"client ip: %s, recv data fail, " \
				"errno: %d, error info: %s", \
				__LINE__, pClientInfo->ip_addr, \
				resp.status, strerror(resp.status));
			break;
		}
		in_buff[nInPackLen] = '\0';

		memcpy(group_name, in_buff, FDFS_GROUP_NAME_MAX_LEN);
		group_name[FDFS_GROUP_NAME_MAX_LEN] = '\0';
		pIpAddr = in_buff + FDFS_GROUP_NAME_MAX_LEN;
		pGroup = tracker_mem_get_group(group_name);
		if (pGroup == NULL)
		{
			logError("file: "__FILE__", line: %d, " \
				"client ip: %s, invalid group_name: %s", \
				__LINE__, pClientInfo->ip_addr, group_name);
			resp.status = ENOENT;
			break;
		}

		resp.status = tracker_mem_delete_storage(pGroup, pIpAddr);
	} while (0);

	resp.cmd = TRACKER_PROTO_CMD_SERVER_RESP;
	if ((result=tcpsenddata_nb(pClientInfo->sock, \
		&resp, sizeof(resp), g_network_timeout)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, send data fail, " \
			"errno: %d, error info: %s", \
			__LINE__, pClientInfo->ip_addr, \
			result, strerror(result));
		return result;
	}

	return resp.status;
}

static int tracker_deal_storage_sync_notify(TrackerClientInfo *pClientInfo, \
				const int64_t nInPackLen)
{
	TrackerStorageSyncReqBody body;
	int status;
	char sync_src_ip_addr[IP_ADDRESS_SIZE];
	bool bSaveStorages;

	do
	{
	if (nInPackLen != sizeof(body))
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd: %d, client ip: %s, package size "INT64_PRINTF_FORMAT" " \
			"is not correct, expect length: %d.", \
			__LINE__, TRACKER_PROTO_CMD_STORAGE_SYNC_NOTIFY, \
			pClientInfo->ip_addr, nInPackLen, sizeof(body));
		status = EINVAL;
		break;
	}

	if ((status=tcprecvdata_nb(pClientInfo->sock, &body, \
			nInPackLen, g_network_timeout)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, recv data fail, " \
			"errno: %d, error info: %s", \
			__LINE__, pClientInfo->ip_addr, \
			status, strerror(status));
		break;
	}

	if (*(body.src_ip_addr) == '\0')
	{
	if (pClientInfo->pStorage->status == FDFS_STORAGE_STATUS_INIT || \
	    pClientInfo->pStorage->status == FDFS_STORAGE_STATUS_WAIT_SYNC || \
	    pClientInfo->pStorage->status == FDFS_STORAGE_STATUS_SYNCING)
	{
		pClientInfo->pStorage->status = FDFS_STORAGE_STATUS_ONLINE;
		pClientInfo->pGroup->version++;
		tracker_save_storages();
	}

		status = 0;
		break;
	}

	bSaveStorages = false;
	if (pClientInfo->pStorage->status == FDFS_STORAGE_STATUS_INIT)
	{
		pClientInfo->pStorage->status = FDFS_STORAGE_STATUS_WAIT_SYNC;
		pClientInfo->pGroup->version++;
		bSaveStorages = true;
	}

	if (pClientInfo->pStorage->psync_src_server == NULL)
	{
		memcpy(sync_src_ip_addr, body.src_ip_addr, IP_ADDRESS_SIZE);
		sync_src_ip_addr[IP_ADDRESS_SIZE-1] = '\0';

		pClientInfo->pStorage->psync_src_server = \
			tracker_mem_get_storage(pClientInfo->pGroup, \
				sync_src_ip_addr);
		if (pClientInfo->pStorage->psync_src_server == NULL)
		{
			logError("file: "__FILE__", line: %d, " \
				"client ip: %s, " \
				"sync src server: %s not exists", \
				__LINE__, pClientInfo->ip_addr, \
				sync_src_ip_addr);
			status = ENOENT;
			break;
		}

		if (pClientInfo->pStorage->psync_src_server->status == \
			FDFS_STORAGE_STATUS_DELETED)
		{
			logError("file: "__FILE__", line: %d, " \
				"client ip: %s, " \
				"sync src server: %s already be deleted", \
				__LINE__, pClientInfo->ip_addr, \
				sync_src_ip_addr);
			status = ENOENT;
			break;
		}

		pClientInfo->pStorage->sync_until_timestamp = \
				(int)buff2long(body.until_timestamp);
		bSaveStorages = true;
	}

	if (bSaveStorages)
	{
		tracker_save_storages();
	}
	status = 0;
	} while (0);

	return tracker_check_and_sync(pClientInfo, status);
}

static int tracker_check_logined(TrackerClientInfo *pClientInfo)
{
	TrackerHeader resp;
	int result;

	if (pClientInfo->pGroup != NULL && pClientInfo->pStorage != NULL)
	{
		return 0;
	}

	memset(&resp, 0, sizeof(resp));
	resp.cmd = TRACKER_PROTO_CMD_STORAGE_RESP;
	resp.status = EACCES;
	if ((result=tcpsenddata_nb(pClientInfo->sock, &resp, sizeof(resp), \
		g_network_timeout)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, send data fail, " \
			"errno: %d, error info: %s.",  \
			__LINE__, pClientInfo->ip_addr, \
			result, strerror(result));
		return result;
	}

	return resp.status;
}

/**
pkg format:
Header
FDFS_GROUP_NAME_MAX_LEN bytes: group_name
**/
static int tracker_deal_server_list_group_storages( \
		TrackerClientInfo *pClientInfo, const int64_t nInPackLen)
{
	TrackerHeader resp;
	char in_buff[FDFS_GROUP_NAME_MAX_LEN + IP_ADDRESS_SIZE];
	char group_name[FDFS_GROUP_NAME_MAX_LEN + 1];
	char *pStorageIp;
	FDFSGroupInfo *pGroup;
	FDFSStorageDetail **ppServer;
	FDFSStorageDetail **ppEnd;
	FDFSStorageStat *pStorageStat;
	TrackerStorageStat stats[FDFS_MAX_SERVERS_EACH_GROUP];
	TrackerStorageStat *pDest;
	FDFSStorageStatBuff *pStatBuff;
	int out_len;
	int result;

	memset(&resp, 0, sizeof(resp));
	pDest = stats;
	pStorageIp = NULL;
	do
	{
		if (nInPackLen < FDFS_GROUP_NAME_MAX_LEN || \
			nInPackLen >= FDFS_GROUP_NAME_MAX_LEN + IP_ADDRESS_SIZE)
		{
			logError("file: "__FILE__", line: %d, " \
				"cmd=%d, client ip: %s, package size " \
				INT64_PRINTF_FORMAT" is not correct, " \
				"expect length >= %d && <=", __LINE__, \
				TRACKER_PROTO_CMD_SERVER_LIST_STORAGE, \
				pClientInfo->ip_addr,  \
				nInPackLen, FDFS_GROUP_NAME_MAX_LEN, \
				FDFS_GROUP_NAME_MAX_LEN + IP_ADDRESS_SIZE);
			resp.status = EINVAL;
			break;
		}

		if ((resp.status=tcprecvdata_nb(pClientInfo->sock, in_buff, \
			nInPackLen, g_network_timeout)) != 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"client ip: %s, recv data fail, " \
				"errno: %d, error info: %s", \
				__LINE__, pClientInfo->ip_addr, \
				resp.status, strerror(resp.status));
			break;
		}

		memcpy(group_name, in_buff, FDFS_GROUP_NAME_MAX_LEN);
		group_name[FDFS_GROUP_NAME_MAX_LEN] = '\0';
		pGroup = tracker_mem_get_group(group_name);
		if (pGroup == NULL)
		{
			logError("file: "__FILE__", line: %d, " \
				"client ip: %s, invalid group_name: %s", \
				__LINE__, pClientInfo->ip_addr, group_name);
			resp.status = ENOENT;
			break;
		}

		if (nInPackLen > FDFS_GROUP_NAME_MAX_LEN)
		{
			pStorageIp = in_buff + FDFS_GROUP_NAME_MAX_LEN;
			*(pStorageIp+(nInPackLen-FDFS_GROUP_NAME_MAX_LEN))='\0';
		}

		memset(stats, 0, sizeof(stats));
		ppEnd = pGroup->sorted_servers + pGroup->count;
		for (ppServer=pGroup->sorted_servers; ppServer<ppEnd; \
			ppServer++)
		{
			if (pStorageIp != NULL && strcmp(pStorageIp, \
				(*ppServer)->ip_addr) != 0)
			{
				continue;
			}

			pStatBuff = &(pDest->stat_buff);
			pStorageStat = &((*ppServer)->stat);
			pDest->status = (*ppServer)->status;
			memcpy(pDest->ip_addr, (*ppServer)->ip_addr, \
				IP_ADDRESS_SIZE);
			if ((*ppServer)->psync_src_server != NULL)
			{
				memcpy(pDest->src_ip_addr, \
					(*ppServer)->psync_src_server->ip_addr,\
					IP_ADDRESS_SIZE);
			}
			long2buff((*ppServer)->up_time, pDest->sz_up_time);
			long2buff((*ppServer)->total_mb, pDest->sz_total_mb);
			long2buff((*ppServer)->free_mb, pDest->sz_free_mb);
			long2buff((*ppServer)->upload_priority, \
				pDest->sz_upload_priority);

			long2buff(pStorageStat->total_upload_count, \
				 pStatBuff->sz_total_upload_count);
			long2buff(pStorageStat->success_upload_count, \
				 pStatBuff->sz_success_upload_count);
			long2buff(pStorageStat->total_set_meta_count, \
				 pStatBuff->sz_total_set_meta_count);
			long2buff(pStorageStat->success_set_meta_count, \
				 pStatBuff->sz_success_set_meta_count);
			long2buff(pStorageStat->total_delete_count, \
				 pStatBuff->sz_total_delete_count);
			long2buff(pStorageStat->success_delete_count, \
				 pStatBuff->sz_success_delete_count);
			long2buff(pStorageStat->total_download_count, \
				 pStatBuff->sz_total_download_count);
			long2buff(pStorageStat->success_download_count, \
				 pStatBuff->sz_success_download_count);
			long2buff(pStorageStat->total_get_meta_count, \
				 pStatBuff->sz_total_get_meta_count);
			long2buff(pStorageStat->success_get_meta_count, \
				 pStatBuff->sz_success_get_meta_count);
			long2buff(pStorageStat->last_source_update, \
				 pStatBuff->sz_last_source_update);
			long2buff(pStorageStat->last_sync_update, \
				 pStatBuff->sz_last_sync_update);
			long2buff(pStorageStat->last_synced_timestamp, \
				 pStatBuff->sz_last_synced_timestamp);
			long2buff(pStorageStat->total_create_link_count, \
				 pStatBuff->sz_total_create_link_count);
			long2buff(pStorageStat->success_create_link_count, \
				 pStatBuff->sz_success_create_link_count);
			long2buff(pStorageStat->total_delete_link_count, \
				 pStatBuff->sz_total_delete_link_count);
			long2buff(pStorageStat->success_delete_link_count, \
				 pStatBuff->sz_success_delete_link_count);
			long2buff(pStorageStat->last_heart_beat_time, \
				 pStatBuff->sz_last_heart_beat_time);

			pDest++;
		}

		if (pStorageIp != NULL && pDest - stats == 0)
		{
			resp.status = ENOENT;
		}
		else
		{
			resp.status = 0;
		}
	} while (0);

	out_len = (pDest - stats) * sizeof(TrackerStorageStat);
	long2buff(out_len, resp.pkg_len);
	resp.cmd = TRACKER_PROTO_CMD_SERVER_RESP;
	if ((result=tcpsenddata_nb(pClientInfo->sock, \
		&resp, sizeof(resp), g_network_timeout)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, send data fail, " \
			"errno: %d, error info: %s", \
			__LINE__, pClientInfo->ip_addr, \
			result, strerror(result));
		return result;
	}

	if (out_len == 0)
	{
		return resp.status;
	}

	if ((result=tcpsenddata_nb(pClientInfo->sock, \
		stats, out_len, g_network_timeout)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, send data fail, " \
			"errno: %d, error info: %s", \
			__LINE__, pClientInfo->ip_addr, \
			result, strerror(result));
		return result;
	}

	return resp.status;
}

/**
pkg format:
Header
FDFS_GROUP_NAME_MAX_LEN bytes: group_name
remain bytes: filename
**/
static int tracker_deal_service_query_fetch_update( \
		TrackerClientInfo *pClientInfo, \
		const byte cmd, const int64_t nInPackLen)
{
	TrackerHeader *pResp;
	char in_buff[FDFS_GROUP_NAME_MAX_LEN + 128];
	char out_buff[sizeof(TrackerHeader) + \
		TRACKER_QUERY_STORAGE_FETCH_BODY_LEN + \
		FDFS_MAX_SERVERS_EACH_GROUP * IP_ADDRESS_SIZE];
	char group_name[FDFS_GROUP_NAME_MAX_LEN + 1];
	char *filename;
	FDFSGroupInfo *pGroup;
	FDFSStorageDetail **ppServer;
	FDFSStorageDetail **ppServerEnd;
	FDFSStorageDetail *ppStoreServers[FDFS_MAX_SERVERS_EACH_GROUP];
	int filename_len;
	int out_len;
	int server_count;
	int result;

	memset(out_buff, 0, sizeof(out_buff));
	pResp = (TrackerHeader *)out_buff;
	pGroup = NULL;
	server_count = 0;
	do
	{
		if (nInPackLen < FDFS_GROUP_NAME_MAX_LEN + 22)
		{
			logError("file: "__FILE__", line: %d, " \
				"cmd=%d, client ip: %s, package size " \
				INT64_PRINTF_FORMAT" is not correct, " \
				"expect length > %d", \
				__LINE__, cmd, \
				pClientInfo->ip_addr,  \
				nInPackLen, FDFS_GROUP_NAME_MAX_LEN+22);
			pResp->status = EINVAL;
			break;
		}

		if (nInPackLen >= sizeof(in_buff))
		{
			logError("file: "__FILE__", line: %d, " \
				"cmd=%d, client ip: %s, package size " \
				INT64_PRINTF_FORMAT" is too large, " \
				"expect length should < %d", \
				__LINE__, cmd, \
				pClientInfo->ip_addr, nInPackLen, \
				sizeof(in_buff));
			pResp->status = EINVAL;
			break;
		}

		if ((pResp->status=tcprecvdata_nb(pClientInfo->sock, in_buff, \
			nInPackLen, g_network_timeout)) != 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"client ip: %s, recv data fail, " \
				"errno: %d, error info: %s", \
				__LINE__, pClientInfo->ip_addr, \
				pResp->status, strerror(pResp->status));
			break;
		}
		in_buff[nInPackLen] = '\0';

		memcpy(group_name, in_buff, FDFS_GROUP_NAME_MAX_LEN);
		group_name[FDFS_GROUP_NAME_MAX_LEN] = '\0';
		filename = in_buff + FDFS_GROUP_NAME_MAX_LEN;
		filename_len = nInPackLen - FDFS_GROUP_NAME_MAX_LEN;

		pResp->status = tracker_mem_get_storage_by_filename(cmd, \
			group_name, filename, filename_len, &pGroup, \
			ppStoreServers, &server_count);
	} while (0);

	pResp->cmd = TRACKER_PROTO_CMD_SERVICE_RESP;
	if (pResp->status == 0)
	{
		char *p;

		out_len = TRACKER_QUERY_STORAGE_FETCH_BODY_LEN + \
				(server_count - 1) * (IP_ADDRESS_SIZE - 1);
		long2buff(out_len, pResp->pkg_len);

		p  = out_buff + sizeof(TrackerHeader);
		memcpy(p, pGroup->group_name, FDFS_GROUP_NAME_MAX_LEN);
		p += FDFS_GROUP_NAME_MAX_LEN;
		memcpy(p, ppStoreServers[0]->ip_addr, IP_ADDRESS_SIZE-1);
		p += IP_ADDRESS_SIZE - 1;
		long2buff(pGroup->storage_port, p);
		p += FDFS_PROTO_PKG_LEN_SIZE;

		if (server_count > 1)
		{
			ppServerEnd = ppStoreServers + server_count;
			for (ppServer=ppStoreServers+1; ppServer<ppServerEnd; \
				ppServer++)
			{
				memcpy(p, (*ppServer)->ip_addr, \
					IP_ADDRESS_SIZE - 1);
				p += IP_ADDRESS_SIZE - 1;
			}
		}
	}
	else
	{
		out_len = 0;
		long2buff(out_len, pResp->pkg_len);
	}

	if ((result=tcpsenddata_nb(pClientInfo->sock, out_buff, \
		sizeof(TrackerHeader) + out_len, g_network_timeout)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, send data fail, " \
			"errno: %d, error info: %s", \
			__LINE__, pClientInfo->ip_addr, \
			result, strerror(result));
		return result;
	}

	return pResp->status;
}

static int tracker_deal_service_query_storage( \
		TrackerClientInfo *pClientInfo, \
		const int64_t nInPackLen, char cmd)
{
	TrackerHeader resp;
	int expect_pkg_len;
	int out_len;
	FDFSGroupInfo *pStoreGroup;
	FDFSGroupInfo **ppFoundGroup;
	FDFSGroupInfo **ppGroup;
	FDFSStorageDetail *pStorageServer;
	char group_name[FDFS_GROUP_NAME_MAX_LEN + 1];
	char out_buff[sizeof(TrackerHeader) + \
		TRACKER_QUERY_STORAGE_STORE_BODY_LEN];
	bool bHaveActiveServer;
	int result;
	int write_path_index;
	int avg_reserved_mb;

	memset(&resp, 0, sizeof(resp));
	pStoreGroup = NULL;
	pStorageServer = NULL;
	write_path_index = 0;
	do
	{
		if (cmd == TRACKER_PROTO_CMD_SERVICE_QUERY_STORE_WITH_GROUP)
		{
			expect_pkg_len = FDFS_GROUP_NAME_MAX_LEN;
		}
		else
		{
			expect_pkg_len = 0;
		}

		if (nInPackLen != expect_pkg_len)
		{
			logError("file: "__FILE__", line: %d, " \
				"cmd=%d, client ip: %s, package size " \
				INT64_PRINTF_FORMAT"is not correct, " \
				"expect length: %d", __LINE__, \
				cmd, pClientInfo->ip_addr, \
				nInPackLen, expect_pkg_len);
			resp.status = EINVAL;
			break;
		}

		if (g_groups.count == 0)
		{
			resp.status = ENOENT;
			break;
		}

		if (cmd == TRACKER_PROTO_CMD_SERVICE_QUERY_STORE_WITH_GROUP)
		{
			if ((resp.status=tcprecvdata_nb(pClientInfo->sock, \
					group_name, nInPackLen, \
					g_network_timeout)) != 0)
			{
				logError("file: "__FILE__", line: %d, " \
					"client ip: %s, recv data fail, " \
					"errno: %d, error info: %s", \
					__LINE__, pClientInfo->ip_addr, \
					resp.status, strerror(resp.status));
				break;
			}
			group_name[nInPackLen] = '\0';

			pStoreGroup = tracker_mem_get_group(group_name);
			if (pStoreGroup == NULL)
			{
				logError("file: "__FILE__", line: %d, " \
					"client ip: %s, invalid group name: %s", \
					__LINE__, pClientInfo->ip_addr, group_name);
				resp.status = ENOENT;
				break;
			}

			if (pStoreGroup->active_count == 0)
			{
				resp.status = ENOENT;
				break;
			}

			if (pStoreGroup->free_mb <= g_storage_reserved_mb)
			{
				resp.status = ENOSPC;
				break;
			}
		}
		else if (g_groups.store_lookup == FDFS_STORE_LOOKUP_ROUND_ROBIN
			||g_groups.store_lookup==FDFS_STORE_LOOKUP_LOAD_BALANCE)
		{
			int write_group_index;

			bHaveActiveServer = false;
			write_group_index = g_groups.current_write_group;
			if (write_group_index >= g_groups.count)
			{
				write_group_index = 0;
			}
			ppFoundGroup = g_groups.sorted_groups + write_group_index;
			if ((*ppFoundGroup)->active_count > 0)
			{
				bHaveActiveServer = true;
				if ((*ppFoundGroup)->free_mb > \
					g_storage_reserved_mb)
				{
					pStoreGroup = *ppFoundGroup;
				}
			}

			if (pStoreGroup == NULL)
			{
				FDFSGroupInfo **ppGroupEnd;
				ppGroupEnd = g_groups.sorted_groups + \
						g_groups.count;
				for (ppGroup=ppFoundGroup+1; \
					ppGroup<ppGroupEnd; ppGroup++)
				{
					if ((*ppGroup)->active_count == 0)
					{
						continue;
					}

					bHaveActiveServer = true;
					if ((*ppGroup)->free_mb > \
						g_storage_reserved_mb)
					{
					pStoreGroup = *ppGroup;
					if (g_groups.store_lookup == \
						FDFS_STORE_LOOKUP_LOAD_BALANCE)
					{
						g_groups.current_write_group = \
						ppGroup-g_groups.sorted_groups;
					}
					break;
					}
				}

				if (pStoreGroup == NULL)
				{
				for (ppGroup=g_groups.sorted_groups; \
					ppGroup<ppFoundGroup; ppGroup++)
				{
					if ((*ppGroup)->active_count == 0)
					{
						continue;
					}

					bHaveActiveServer = true;
					if ((*ppGroup)->free_mb > \
						g_storage_reserved_mb)
					{
					pStoreGroup = *ppGroup;
					if (g_groups.store_lookup == \
						FDFS_STORE_LOOKUP_LOAD_BALANCE)
					{
						g_groups.current_write_group = \
						ppGroup-g_groups.sorted_groups;
					}
					break;
					}
				}
				}

				if (pStoreGroup == NULL)
				{
					if (bHaveActiveServer)
					{
						resp.status = ENOSPC;
					}
					else
					{
						resp.status = ENOENT;
					}
					break;
				}
			}

			if (g_groups.store_lookup == FDFS_STORE_LOOKUP_ROUND_ROBIN)
			{
				g_groups.current_write_group++;
				if (g_groups.current_write_group >= g_groups.count)
				{
					g_groups.current_write_group = 0;
				}
			}
		}
		else if (g_groups.store_lookup == FDFS_STORE_LOOKUP_SPEC_GROUP)
		{
			if (g_groups.pStoreGroup == NULL || \
				g_groups.pStoreGroup->active_count == 0)
			{
				resp.status = ENOENT;
				break;
			}

			if (g_groups.pStoreGroup->free_mb <= \
				g_storage_reserved_mb)
			{
				resp.status = ENOSPC;
				break;
			}

			pStoreGroup = g_groups.pStoreGroup;
		}
		else
		{
			resp.status = EINVAL;
			break;
		}

		if (pStoreGroup->store_path_count <= 0)
		{
			resp.status = ENOENT;
			break;
		}

		pStorageServer = tracker_get_writable_storage(pStoreGroup);
		if (pStorageServer == NULL)
		{
			resp.status = ENOENT;
			break;
		}

		write_path_index = pStorageServer->current_write_path;
		if (write_path_index >= pStoreGroup->store_path_count)
		{
			write_path_index = 0;
		}

		avg_reserved_mb = g_storage_reserved_mb / \
				pStoreGroup->store_path_count;
		if (pStorageServer->path_free_mbs[write_path_index] <= \
				avg_reserved_mb)
		{
			int i;
			for (i=0; i<pStoreGroup->store_path_count; i++)
			{
				if (pStorageServer->path_free_mbs[i]
				 	> avg_reserved_mb)
				{
					pStorageServer->current_write_path = i;
					write_path_index = i;
					break;
				}
			}

			if (i == pStoreGroup->store_path_count)
			{
				resp.status = ENOSPC;
				break;
			}
		}

		if (g_groups.store_path == FDFS_STORE_PATH_ROUND_ROBIN)
		{
			pStorageServer->current_write_path++;
			if (pStorageServer->current_write_path >= \
				pStoreGroup->store_path_count)
			{
				pStorageServer->current_write_path = 0;
			}
		}

		/*
		//printf("pStoreGroup->current_write_server: %d, " \
			"pStoreGroup->active_count=%d\n", \
			pStoreGroup->current_write_server, \
			pStoreGroup->active_count);
		*/

		resp.status = 0;
	} while (0);

	resp.cmd = TRACKER_PROTO_CMD_SERVICE_RESP;
	if (resp.status == 0)
	{
		out_len = TRACKER_QUERY_STORAGE_STORE_BODY_LEN;
		long2buff(out_len, resp.pkg_len);

		memcpy(out_buff, &resp, sizeof(resp));
		memcpy(out_buff + sizeof(resp), pStoreGroup->group_name, \
				FDFS_GROUP_NAME_MAX_LEN);
		memcpy(out_buff + sizeof(resp) + FDFS_GROUP_NAME_MAX_LEN, \
				pStorageServer->ip_addr, IP_ADDRESS_SIZE-1);

		long2buff(pStoreGroup->storage_port, out_buff + sizeof(resp) + \
			FDFS_GROUP_NAME_MAX_LEN + IP_ADDRESS_SIZE-1);

		*(out_buff + sizeof(resp) + FDFS_GROUP_NAME_MAX_LEN + \
		    IP_ADDRESS_SIZE - 1 + FDFS_PROTO_PKG_LEN_SIZE) = \
				(char)write_path_index;
	}
	else
	{
		out_len = 0;
		long2buff(out_len, resp.pkg_len);
		memcpy(out_buff, &resp, sizeof(resp));
	}

	if ((result=tcpsenddata_nb(pClientInfo->sock, \
		out_buff, sizeof(resp) + out_len, g_network_timeout)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, send data fail, " \
			"errno: %d, error info: %s", \
			__LINE__, pClientInfo->ip_addr, \
			result, strerror(result));
		return result;
	}

	return resp.status;
}

static int tracker_deal_server_list_groups(TrackerClientInfo *pClientInfo, \
				const int64_t nInPackLen)
{
	TrackerHeader resp;
	FDFSGroupInfo **ppGroup;
	FDFSGroupInfo **ppEnd;
	TrackerGroupStat groupStats[FDFS_MAX_GROUPS];
	TrackerGroupStat *pDest;
	int out_len;
	int result;

	memset(&resp, 0, sizeof(resp));
	pDest = groupStats;
	do
	{
		if (nInPackLen != 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"cmd=%d, client ip: %s, package size " \
				INT64_PRINTF_FORMAT" is not correct, " \
				"expect length: 0", __LINE__, \
				TRACKER_PROTO_CMD_SERVER_LIST_GROUP, \
				pClientInfo->ip_addr, nInPackLen);
			resp.status = EINVAL;
			break;
		}

		ppEnd = g_groups.sorted_groups + g_groups.count;
		for (ppGroup=g_groups.sorted_groups; ppGroup<ppEnd; ppGroup++)
		{
			memcpy(pDest->group_name, (*ppGroup)->group_name, \
				FDFS_GROUP_NAME_MAX_LEN + 1);
			long2buff((*ppGroup)->free_mb, pDest->sz_free_mb);
			long2buff((*ppGroup)->count, pDest->sz_count);
			long2buff((*ppGroup)->storage_port, \
				pDest->sz_storage_port);
			long2buff((*ppGroup)->storage_http_port, \
				pDest->sz_storage_http_port);
			long2buff((*ppGroup)->active_count, \
				pDest->sz_active_count);
			long2buff((*ppGroup)->current_write_server, \
				pDest->sz_current_write_server);
			long2buff((*ppGroup)->store_path_count, \
				pDest->sz_store_path_count);
			long2buff((*ppGroup)->subdir_count_per_path, \
				pDest->sz_subdir_count_per_path);
			pDest++;
		}

		resp.status = 0;
	} while (0);

	out_len = (pDest - groupStats) * sizeof(TrackerGroupStat);
	long2buff(out_len, resp.pkg_len);
	resp.cmd = TRACKER_PROTO_CMD_SERVER_RESP;
	if ((result=tcpsenddata_nb(pClientInfo->sock, \
		&resp, sizeof(resp), g_network_timeout)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, send data fail, " \
			"errno: %d, error info: %s", \
			__LINE__, pClientInfo->ip_addr, \
			result, strerror(result));
		return result;
	}

	if (out_len == 0)
	{
		return resp.status;
	}

	if ((result=tcpsenddata_nb(pClientInfo->sock, \
		groupStats, out_len, g_network_timeout)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, send data fail, " \
			"errno: %d, error info: %s", \
			__LINE__, pClientInfo->ip_addr, \
			result, strerror(result));
		return result;
	}

	return resp.status;
}

static int tracker_deal_storage_sync_src_req(TrackerClientInfo *pClientInfo, \
				const int64_t nInPackLen)
{
	char out_buff[sizeof(TrackerHeader)+sizeof(TrackerStorageSyncReqBody)];
	char dest_ip_addr[IP_ADDRESS_SIZE];
	TrackerHeader *pResp;
	TrackerStorageSyncReqBody *pBody;
	FDFSStorageDetail *pDestStorage;
	int out_len;
	int result;

	memset(out_buff, 0, sizeof(out_buff));
	pResp = (TrackerHeader *)out_buff;
	pBody = (TrackerStorageSyncReqBody *)(out_buff + sizeof(TrackerHeader));
	out_len = sizeof(TrackerHeader);
	do
	{
		if (nInPackLen != IP_ADDRESS_SIZE)
		{
			logError("file: "__FILE__", line: %d, " \
				"cmd=%d, client ip: %s, package size " \
				INT64_PRINTF_FORMAT" is not correct, " \
				"expect length: %d", __LINE__, \
				TRACKER_PROTO_CMD_STORAGE_SYNC_SRC_REQ, \
				pClientInfo->ip_addr, nInPackLen, \
				IP_ADDRESS_SIZE);
			pResp->status = EINVAL;
			break;
		}

		if ((pResp->status=tcprecvdata_nb(pClientInfo->sock, \
			dest_ip_addr, nInPackLen, g_network_timeout)) != 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"cmd=%d, client ip addr: %s, recv data fail, " \
				"errno: %d, error info: %s.", \
				__LINE__, \
				TRACKER_PROTO_CMD_STORAGE_SYNC_SRC_REQ, \
				pClientInfo->ip_addr, \
				pResp->status, strerror(pResp->status));
			break;
		}

		dest_ip_addr[IP_ADDRESS_SIZE-1] = '\0';
		pDestStorage = tracker_mem_get_storage(pClientInfo->pGroup, \
				dest_ip_addr);
		if (pDestStorage == NULL)
		{
			pResp->status = ENOENT;
			break;
		}

		if (pDestStorage->status == FDFS_STORAGE_STATUS_INIT || \
			pDestStorage->status == FDFS_STORAGE_STATUS_DELETED)
		{
			pResp->status = ENOENT;
			break;
		}

		if (pDestStorage->psync_src_server != NULL)
		{
			strcpy(pBody->src_ip_addr, \
				pDestStorage->psync_src_server->ip_addr);
			long2buff(pDestStorage->sync_until_timestamp, \
				pBody->until_timestamp);
			out_len += sizeof(TrackerStorageSyncReqBody);
		}

		pResp->status = 0;
	} while (0);

	//printf("deal sync request, status=%d\n", pResp->status);

	long2buff(out_len - (int)sizeof(TrackerHeader), pResp->pkg_len);
	pResp->cmd = TRACKER_PROTO_CMD_SERVER_RESP;
	if ((result=tcpsenddata_nb(pClientInfo->sock, \
		out_buff, out_len, g_network_timeout)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, send data fail, " \
			"errno: %d, error info: %s", \
			__LINE__, pClientInfo->ip_addr, \
			result, strerror(result));
		return result;
	}

	return 0;
}

static int tracker_deal_storage_sync_dest_req(TrackerClientInfo *pClientInfo, \
				const int64_t nInPackLen)
{
	char out_buff[sizeof(TrackerHeader)+sizeof(TrackerStorageSyncReqBody)];
	TrackerHeader *pResp;
	TrackerStorageSyncReqBody *pBody;
	FDFSStorageDetail *pSrcStorage;
	FDFSStorageDetail *pServer;
	FDFSStorageDetail *pServerEnd;
	int out_len;
	int sync_until_timestamp;
	int source_count;
	int result;

	sync_until_timestamp = 0;
	memset(out_buff, 0, sizeof(out_buff));
	pResp = (TrackerHeader *)out_buff;
	pBody = (TrackerStorageSyncReqBody *)(out_buff + sizeof(TrackerHeader));
	out_len = sizeof(TrackerHeader);
	pSrcStorage = NULL;
	do
	{
		if (nInPackLen != 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"cmd=%d, client ip: %s, package size " \
				INT64_PRINTF_FORMAT" is not correct, " \
				"expect length: 0", \
				__LINE__, \
				TRACKER_PROTO_CMD_STORAGE_SYNC_DEST_REQ, \
				pClientInfo->ip_addr, nInPackLen);
			pResp->status = EINVAL;
			break;
		}

		if (pClientInfo->pGroup->count <= 1 || \
			tracker_get_group_success_upload_count( \
				pClientInfo->pGroup) <= 0)
		{
			pResp->status = 0;
			break;
		}

        	source_count = 0;
		pServerEnd = pClientInfo->pGroup->all_servers + \
				pClientInfo->pGroup->count;
		for (pServer=pClientInfo->pGroup->all_servers; \
			pServer<pServerEnd; pServer++)
		{
			if (strcmp(pServer->ip_addr, \
				pClientInfo->pStorage->ip_addr) == 0)
			{
				continue;
			}

			if (pServer->status != FDFS_STORAGE_STATUS_DELETED
			&& pServer->status != FDFS_STORAGE_STATUS_INIT
			&& pServer->status != FDFS_STORAGE_STATUS_WAIT_SYNC)
			{
				source_count++;
			}
		}
		if (source_count == 0)
		{
			pResp->status = 0;
			break;
		}

		pSrcStorage = tracker_get_group_sync_src_server( \
			pClientInfo->pGroup, pClientInfo->pStorage);
		if (pSrcStorage == NULL)
		{
			pResp->status = ENOENT;
			break;
		}

		sync_until_timestamp = (int)time(NULL);
		strcpy(pBody->src_ip_addr, pSrcStorage->ip_addr);

		long2buff(sync_until_timestamp, pBody->until_timestamp);
		out_len += sizeof(TrackerStorageSyncReqBody);
		pResp->status = 0;
	} while (0);

	//printf("deal sync request, status=%d\n", pResp->status);

	long2buff(out_len - (int)sizeof(TrackerHeader), pResp->pkg_len);
	pResp->cmd = TRACKER_PROTO_CMD_SERVER_RESP;
	if ((result=tcpsenddata_nb(pClientInfo->sock, \
		out_buff, out_len, g_network_timeout)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, send data fail, " \
			"errno: %d, error info: %s", \
			__LINE__, pClientInfo->ip_addr, \
			result, strerror(result));
		return result;
	}

	if (pSrcStorage == NULL)
	{
		if (pResp->status == 0)
		{
			pClientInfo->pStorage->status = \
				FDFS_STORAGE_STATUS_ONLINE;
			pClientInfo->pGroup->version++;
			tracker_save_storages();
		}

		return pResp->status;
	}

	if ((result=tcprecvdata_nb(pClientInfo->sock, pResp, \
		sizeof(TrackerHeader), g_network_timeout)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"cmd=%d, client ip addr: %s, recv data fail, " \
			"errno: %d, error info: %s.", \
			__LINE__, \
			TRACKER_PROTO_CMD_STORAGE_SYNC_DEST_REQ, \
			pClientInfo->ip_addr, \
			result, strerror(result));
		return result;
	}

	if (pResp->cmd != TRACKER_PROTO_CMD_STORAGE_RESP)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip addr: %s, " \
			"recv storage confirm fail, resp cmd: %d, " \
			"expect cmd: %d",  \
			__LINE__, pClientInfo->ip_addr, \
			pResp->cmd, TRACKER_PROTO_CMD_STORAGE_RESP);
		return EINVAL;
	}

	if (pResp->status != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip addr: %s, " \
			"recv storage confirm fail, resp status: %d, " \
			"expect status: 0",  \
			__LINE__, pClientInfo->ip_addr, pResp->status);
		return pResp->status;
	}

	pClientInfo->pStorage->psync_src_server = pSrcStorage;
	pClientInfo->pStorage->sync_until_timestamp = sync_until_timestamp;
	pClientInfo->pStorage->status = FDFS_STORAGE_STATUS_WAIT_SYNC;
	pClientInfo->pGroup->version++;

	tracker_save_storages();
	return 0;
}

static int tracker_deal_storage_sync_dest_query(TrackerClientInfo *pClientInfo,\
				const int64_t nInPackLen)
{
	char out_buff[sizeof(TrackerHeader)+sizeof(TrackerStorageSyncReqBody)];
	TrackerHeader *pResp;
	TrackerStorageSyncReqBody *pBody;
	int out_len;
	FDFSStorageDetail *pSrcStorage;
	int result;

	memset(out_buff, 0, sizeof(out_buff));
	pResp = (TrackerHeader *)out_buff;
	pBody = (TrackerStorageSyncReqBody *)(out_buff + sizeof(TrackerHeader));
	out_len = sizeof(TrackerHeader);
	pSrcStorage = pClientInfo->pStorage->psync_src_server;
	do
	{
		if (nInPackLen != 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"cmd=%d, client ip: %s, package size " \
				INT64_PRINTF_FORMAT" is not correct, " \
				"expect length: 0", \
				__LINE__, \
				TRACKER_PROTO_CMD_STORAGE_SYNC_DEST_QUERY, \
				pClientInfo->ip_addr, nInPackLen);
			pResp->status = EINVAL;
			break;
		}

		if (pSrcStorage != NULL)
		{
			strcpy(pBody->src_ip_addr, pSrcStorage->ip_addr);

			long2buff(pClientInfo->pStorage->sync_until_timestamp, \
				pBody->until_timestamp);
			out_len += sizeof(TrackerStorageSyncReqBody);
		}

		pResp->status = 0;
	} while (0);

	long2buff(out_len - (int)sizeof(TrackerHeader), pResp->pkg_len);
	pResp->cmd = TRACKER_PROTO_CMD_SERVER_RESP;
	if ((result=tcpsenddata_nb(pClientInfo->sock, \
		out_buff, out_len, g_network_timeout)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"client ip: %s, send data fail, " \
			"errno: %d, error info: %s", \
			__LINE__, pClientInfo->ip_addr, \
			result, strerror(result));
		return result;
	}

	return pResp->status;
}

static void tracker_find_max_free_space_group()
{
	FDFSGroupInfo **ppGroup;
	FDFSGroupInfo **ppGroupEnd;
	FDFSGroupInfo **ppMaxGroup;

	ppMaxGroup = NULL;
	ppGroupEnd = g_groups.sorted_groups + g_groups.count;
	for (ppGroup=g_groups.sorted_groups; \
		ppGroup<ppGroupEnd; ppGroup++)
	{
		if ((*ppGroup)->active_count > 0)
		{
			if (ppMaxGroup == NULL)
			{
				ppMaxGroup = ppGroup;
			}
			else if ((*ppGroup)->free_mb > (*ppMaxGroup)->free_mb)
			{
				ppMaxGroup = ppGroup;
			}
		}
	}

	if (ppMaxGroup == NULL)
	{
		return;
	}

	g_groups.current_write_group = ppMaxGroup - g_groups.sorted_groups;
}

static void tracker_find_min_free_space(FDFSGroupInfo *pGroup)
{
	FDFSStorageDetail **ppServerEnd;
	FDFSStorageDetail **ppServer;

	if (pGroup->active_count == 0)
	{
		return;
	}

	pGroup->free_mb = (*(pGroup->active_servers))->free_mb;
	ppServerEnd = pGroup->active_servers + pGroup->active_count;
	for (ppServer=pGroup->active_servers+1; \
		ppServer<ppServerEnd; ppServer++)
	{
		if ((*ppServer)->free_mb < pGroup->free_mb)
		{
			pGroup->free_mb = (*ppServer)->free_mb;
		}
	}
}

static int tracker_deal_storage_sync_report(TrackerClientInfo *pClientInfo, \
			const int64_t nInPackLen)
{
	int status;
	char in_buff[512];
	char *p;
	char *pEnd;
	char *src_ip_addr;
	int sync_timestamp;
	int src_index;
	int dest_index;
	FDFSStorageDetail *pSrcStorage;

	do 
	{
		if (nInPackLen <= 0 || nInPackLen > sizeof(in_buff) \
			|| nInPackLen % (IP_ADDRESS_SIZE + 4) != 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"cmd=%d, client ip: %s, package size " \
				INT64_PRINTF_FORMAT" is not correct", \
				__LINE__, \
				TRACKER_PROTO_CMD_STORAGE_SYNC_REPORT, \
				pClientInfo->ip_addr, nInPackLen);
			status = EINVAL;
			break;
		}

		if ((status=tcprecvdata_nb(pClientInfo->sock, in_buff, \
			nInPackLen, g_network_timeout)) != 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"cmd=%d, client ip addr: %s, recv data fail, " \
				"errno: %d, error info: %s.", \
				__LINE__, \
				TRACKER_PROTO_CMD_STORAGE_SYNC_REPORT, \
				pClientInfo->ip_addr, \
				status, strerror(status));
			break;
		}

		tracker_check_dirty(pClientInfo);

		dest_index = pClientInfo->pStorage - \
				pClientInfo->pGroup->all_servers;
		if (dest_index < 0 || dest_index >= pClientInfo->pGroup->count)
		{
			status = 0;
			break;
		}

		if (g_groups.store_server == FDFS_STORE_SERVER_ROUND_ROBIN)
		{
			int min_synced_timestamp;

			min_synced_timestamp = 0;
			pEnd = in_buff + nInPackLen;
			for (p=in_buff; p<pEnd; p += (IP_ADDRESS_SIZE + 4))
			{
				sync_timestamp = buff2int(p + IP_ADDRESS_SIZE);
				if (sync_timestamp <= 0)
				{
					continue;
				}

				src_ip_addr = p;
				*(src_ip_addr + (IP_ADDRESS_SIZE - 1)) = '\0';
 				pSrcStorage = tracker_mem_get_storage( \
					pClientInfo->pGroup, src_ip_addr);
				if (pSrcStorage == NULL)
				{
					continue;
				}
				if (pSrcStorage->status != FDFS_STORAGE_STATUS_ACTIVE)
				{
					continue;
				}

				src_index = pSrcStorage - 
						pClientInfo->pGroup->all_servers;
				if (src_index == dest_index || src_index < 0 || \
					src_index >= pClientInfo->pGroup->count)
				{
					continue;
				}

				pClientInfo->pGroup->last_sync_timestamps \
					[src_index][dest_index] = sync_timestamp;

				if (min_synced_timestamp == 0)
				{
					min_synced_timestamp = sync_timestamp;
				}
				else if (sync_timestamp < min_synced_timestamp)
				{
					min_synced_timestamp = sync_timestamp;
				}
			}

			if (min_synced_timestamp > 0)
			{
				pClientInfo->pStorage->stat.last_synced_timestamp = \
					min_synced_timestamp;
			}
		}
		else
		{
			int max_synced_timestamp;

			max_synced_timestamp = pClientInfo->pStorage->stat.\
						last_synced_timestamp;
			pEnd = in_buff + nInPackLen;
			for (p=in_buff; p<pEnd; p += (IP_ADDRESS_SIZE + 4))
			{
				sync_timestamp = buff2int(p + IP_ADDRESS_SIZE);
				if (sync_timestamp <= 0)
				{
					continue;
				}

				src_ip_addr = p;
				*(src_ip_addr + (IP_ADDRESS_SIZE - 1)) = '\0';
 				pSrcStorage = tracker_mem_get_storage( \
					pClientInfo->pGroup, src_ip_addr);
				if (pSrcStorage == NULL)
				{
					continue;
				}
				if (pSrcStorage->status != FDFS_STORAGE_STATUS_ACTIVE)
				{
					continue;
				}

				src_index = pSrcStorage - 
						pClientInfo->pGroup->all_servers;
				if (src_index == dest_index || src_index < 0 || \
					src_index >= pClientInfo->pGroup->count)
				{
					continue;
				}

				pClientInfo->pGroup->last_sync_timestamps \
					[src_index][dest_index] = sync_timestamp;

				if (sync_timestamp > max_synced_timestamp)
				{
					max_synced_timestamp = sync_timestamp;
				}
			}

			pClientInfo->pStorage->stat.last_synced_timestamp = \
					max_synced_timestamp;
		}

		if (++g_storage_sync_time_chg_count % \
			TRACKER_SYNC_TO_FILE_FREQ == 0)
		{
			status = tracker_save_sync_timestamps();
		}
		else
		{
			status = 0;
		}

		//printf("storage_sync_time_chg_count=%d\n", g_storage_sync_time_chg_count);
	} while (0);

	//printf("deal storage report, status=%d\n", status);
	return tracker_check_and_sync(pClientInfo, status);
}

static int tracker_deal_storage_df_report(TrackerClientInfo *pClientInfo, \
				const int64_t nInPackLen)
{
	int status;
	int result;
	int i;
	char in_buff[sizeof(TrackerStatReportReqBody) * 16];
	char *pBuff;
	TrackerStatReportReqBody *pStatBuff;
	int64_t *path_total_mbs;
	int64_t *path_free_mbs;
	int64_t old_free_mb;

	pBuff = in_buff;
	do
	{
		if (nInPackLen != sizeof(TrackerStatReportReqBody) * \
			pClientInfo->pGroup->store_path_count)
		{
			logError("file: "__FILE__", line: %d, " \
				"cmd=%d, client ip: %s, package size " \
				INT64_PRINTF_FORMAT" is not correct, " \
				"expect length: %d", __LINE__, \
				TRACKER_PROTO_CMD_STORAGE_REPORT, \
				pClientInfo->ip_addr, nInPackLen, \
				sizeof(TrackerStatReportReqBody) * \
                        	pClientInfo->pGroup->store_path_count);
			status = EINVAL;
			break;
		}

		if (nInPackLen > sizeof(in_buff))
		{
			pBuff = (char *)malloc(nInPackLen);
			if (pBuff == NULL)
			{
				logError("file: "__FILE__", line: %d, " \
					"malloc %d bytes fail, " \
					"errno: %d, error info: %s", __LINE__, \
					(int)nInPackLen, errno,strerror(errno));
				status = errno != 0 ? errno : ENOMEM;
				break;
			}
		}
		if ((status=tcprecvdata_nb(pClientInfo->sock, pBuff, \
			nInPackLen, g_network_timeout)) != 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"cmd=%d, client ip addr: %s, recv data fail, " \
				"errno: %d, error info: %s.", \
				__LINE__, \
				TRACKER_PROTO_CMD_STORAGE_REPORT, \
				pClientInfo->ip_addr, \
				status, strerror(status));
			break;
		}

		old_free_mb = pClientInfo->pStorage->free_mb;
		path_total_mbs = pClientInfo->pStorage->path_total_mbs;
		path_free_mbs = pClientInfo->pStorage->path_free_mbs;
		pClientInfo->pStorage->total_mb = 0;
		pClientInfo->pStorage->free_mb = 0;

		pStatBuff = (TrackerStatReportReqBody *)pBuff;
		for (i=0; i<pClientInfo->pGroup->store_path_count; i++)
		{
			path_total_mbs[i] = buff2long(pStatBuff->sz_total_mb);
			path_free_mbs[i] = buff2long(pStatBuff->sz_free_mb);

			pClientInfo->pStorage->total_mb += path_total_mbs[i];
			pClientInfo->pStorage->free_mb += path_free_mbs[i];

			if (g_groups.store_path == FDFS_STORE_PATH_LOAD_BALANCE
				&& path_free_mbs[i] > path_free_mbs[ \
				pClientInfo->pStorage->current_write_path])
			{
				pClientInfo->pStorage->current_write_path = i;
			}

			pStatBuff++;
		}

		if ((pClientInfo->pGroup->free_mb == 0) ||
			(pClientInfo->pStorage->free_mb < \
				pClientInfo->pGroup->free_mb))
		{
			pClientInfo->pGroup->free_mb = \
				pClientInfo->pStorage->free_mb;
		}
		else if (pClientInfo->pStorage->free_mb > old_free_mb)
		{
			tracker_find_min_free_space(pClientInfo->pGroup);
		}

		if (g_groups.store_lookup == \
			FDFS_STORE_LOOKUP_LOAD_BALANCE)
		{
			if ((result=pthread_mutex_lock(&lb_thread_lock)) != 0)
			{
				logError("file: "__FILE__", line: %d, " \
				"call pthread_mutex_lock fail, " \
				"errno: %d, error info: %s", \
				__LINE__, result, strerror(result));
			}
			tracker_find_max_free_space_group();
			if ((result=pthread_mutex_unlock(&lb_thread_lock)) != 0)
			{
				logError("file: "__FILE__", line: %d, " \
					"call pthread_mutex_unlock fail, " \
					"errno: %d, error info: %s", \
					__LINE__, result, strerror(result));
			}
		}

		status = 0;

		/*
		//logInfo("storage: %s:%d, total_mb=%dMB, free_mb=%dMB\n", \
			pClientInfo->pStorage->ip_addr, \
			pClientInfo->pGroup->storage_port, \
			pClientInfo->pStorage->total_mb, \
			pClientInfo->pStorage->free_mb);
		*/

	} while (0);

	if (pBuff != in_buff && pBuff != NULL)
	{
		free(pBuff);
	}

	if (status == 0)
	{
		tracker_check_dirty(pClientInfo);
		tracker_mem_active_store_server(pClientInfo->pGroup, \
				pClientInfo->pStorage);
	}

	return tracker_check_and_sync(pClientInfo, status);
}

static int tracker_deal_storage_beat(TrackerClientInfo *pClientInfo, \
				const int64_t nInPackLen)
{
	int status;
	FDFSStorageStatBuff statBuff;
	FDFSStorageStat *pStat;

	do 
	{
		if (nInPackLen == 0)
		{
			status = 0;
			break;
		}

		if (nInPackLen != sizeof(FDFSStorageStatBuff))
		{
			logError("file: "__FILE__", line: %d, " \
				"cmd=%d, client ip: %s, package size "INT64_PRINTF_FORMAT" " \
				"is not correct, " \
				"expect length: 0 or %d", \
				__LINE__, \
				TRACKER_PROTO_CMD_STORAGE_BEAT, \
				pClientInfo->ip_addr, nInPackLen, \
				sizeof(FDFSStorageStatBuff));
			status = EINVAL;
			break;
		}

		if ((status=tcprecvdata_nb(pClientInfo->sock, &statBuff, \
			sizeof(FDFSStorageStatBuff), g_network_timeout)) != 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"cmd=%d, client ip addr: %s, recv data fail, " \
				"errno: %d, error info: %s.", \
				__LINE__, \
				TRACKER_PROTO_CMD_STORAGE_BEAT, \
				pClientInfo->ip_addr, \
				status, strerror(status));
			break;
		}

		pStat = &(pClientInfo->pStorage->stat);

		pStat->total_upload_count = \
			buff2long(statBuff.sz_total_upload_count);
		pStat->success_upload_count = \
			buff2long(statBuff.sz_success_upload_count);
		pStat->total_download_count = \
			buff2long(statBuff.sz_total_download_count);
		pStat->success_download_count = \
			buff2long(statBuff.sz_success_download_count);
		pStat->total_set_meta_count = \
			buff2long(statBuff.sz_total_set_meta_count);
		pStat->success_set_meta_count = \
			buff2long(statBuff.sz_success_set_meta_count);
		pStat->total_delete_count = \
			buff2long(statBuff.sz_total_delete_count);
		pStat->success_delete_count = \
			buff2long(statBuff.sz_success_delete_count);
		pStat->total_get_meta_count = \
			buff2long(statBuff.sz_total_get_meta_count);
		pStat->success_get_meta_count = \
			buff2long(statBuff.sz_success_get_meta_count);
		pStat->last_source_update = \
			buff2long(statBuff.sz_last_source_update);
		pStat->last_sync_update = \
			buff2long(statBuff.sz_last_sync_update);
		pStat->total_create_link_count = \
			buff2long(statBuff.sz_total_create_link_count);
		pStat->success_create_link_count = \
			buff2long(statBuff.sz_success_create_link_count);
		pStat->total_delete_link_count = \
			buff2long(statBuff.sz_total_delete_link_count);
		pStat->success_delete_link_count = \
			buff2long(statBuff.sz_success_delete_link_count);

		if (++g_storage_stat_chg_count % TRACKER_SYNC_TO_FILE_FREQ == 0)
		{
			status = tracker_save_storages();
		}
		else
		{
			status = 0;
		}

		//printf("g_storage_stat_chg_count=%d\n", g_storage_stat_chg_count);

	} while (0);

	if (status == 0)
	{
		tracker_check_dirty(pClientInfo);
		tracker_mem_active_store_server(pClientInfo->pGroup, \
				pClientInfo->pStorage);
		pClientInfo->pStorage->stat.last_heart_beat_time = time(NULL);

	}

	//printf("deal heart beat, status=%d\n", status);
	return tracker_check_and_sync(pClientInfo, status);
}


void* tracker_thread_entrance(void* arg)
{
/*
package format:
8 bytes length (hex string)
1 bytes cmd (char)
1 bytes status(char)
data buff (struct)
*/
	TrackerClientInfo client_info;
	TrackerHeader header;
	int result;
	int64_t nInPackLen;
	int count;
	int recv_bytes;
	int log_level;
	in_addr_t client_ip;
	int server_sock;
	
	server_sock = (int)arg;

	while (g_continue_flag)
	{
	if ((result=pthread_mutex_lock(&tracker_thread_lock)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"call pthread_mutex_lock fail, " \
			"errno: %d, error info: %s", \
			__LINE__, result, strerror(result));
	}

	if (!g_continue_flag)
	{
		pthread_mutex_unlock(&tracker_thread_lock);
		break;
	}

	memset(&client_info, 0, sizeof(client_info));
	client_info.sock = nbaccept(server_sock, 1 * 60, &result);
	if (pthread_mutex_unlock(&tracker_thread_lock) != 0)
	{
		logError("file: "__FILE__", line: %d, "   \
			"call pthread_mutex_unlock fail", \
			__LINE__);
	}
	if(client_info.sock < 0) //error
	{
		if (result == ETIMEDOUT || result == EINTR || \
			result == EAGAIN)
		{
			continue;
		}
			
		if(result == EBADF)
		{
			logError("file: "__FILE__", line: %d, " \
				"accept failed, " \
				"errno: %d, error info: %s", \
				__LINE__, result, strerror(result));
			break;
		}
			
		logError("file: "__FILE__", line: %d, " \
			"accept failed, errno: %d, error info: %s", \
			__LINE__, result, strerror(result));
		continue;
	}
	
	client_ip = getPeerIpaddr(client_info.sock, \
				client_info.ip_addr, IP_ADDRESS_SIZE);
	if (g_allow_ip_count >= 0)
	{
		if (bsearch(&client_ip, g_allow_ip_addrs, g_allow_ip_count, \
			sizeof(in_addr_t), cmp_by_ip_addr_t) == NULL)
		{
			logError("file: "__FILE__", line: %d, " \
				"ip addr %s is not allowed to access", \
				__LINE__, client_info.ip_addr);

			close(client_info.sock);
			continue;
		}
	}

	if (tcpsetnonblockopt(client_info.sock) != 0)
	{
		close(client_info.sock);
		continue;
	}

	count = 0;
	while (g_continue_flag)
	{
		result = tcprecvdata_nb_ex(client_info.sock, &header, \
			sizeof(header), g_network_timeout, &recv_bytes);
		if (result == ETIMEDOUT && count > 0)
		{
			continue;
		}

		if (result != 0)
		{
			if (result == ENOTCONN && recv_bytes == 0)
			{
				log_level = LOG_DEBUG;
			}
			else
			{
				log_level = LOG_ERR;
			}

			if (log_level <= g_log_level)
			{
			log_it(log_level, "file: "__FILE__", line: %d, " \
				"client ip: %s, recv data fail, " \
				"errno: %d, error info: %s", \
				__LINE__, client_info.ip_addr, \
				result, strerror(result));
			}

			break;
		}

		nInPackLen = buff2long(header.pkg_len);

		tracker_check_dirty(&client_info);

		switch(header.cmd)
		{
		case TRACKER_PROTO_CMD_STORAGE_BEAT:
			if ((result=tracker_check_logined(&client_info)) != 0)
			{
				break;
			}

			result = tracker_deal_storage_beat(&client_info, \
				nInPackLen);
			break;
		case TRACKER_PROTO_CMD_STORAGE_SYNC_REPORT:
			if ((result=tracker_check_logined(&client_info)) != 0)
			{
				break;
			}

			result = tracker_deal_storage_sync_report( \
				&client_info, nInPackLen);
			break;
		case TRACKER_PROTO_CMD_STORAGE_REPORT:
			if ((result=tracker_check_logined(&client_info)) != 0)
			{
				break;
			}

			result = tracker_deal_storage_df_report(&client_info, \
				nInPackLen);
			break;
		case TRACKER_PROTO_CMD_STORAGE_JOIN:
			result = tracker_deal_storage_join(&client_info, \
				nInPackLen);
			break;
		case TRACKER_PROTO_CMD_STORAGE_REPLICA_CHG:
			if ((result=tracker_check_logined(&client_info)) != 0)
			{
				break;
			}

			result = tracker_deal_storage_replica_chg( \
				&client_info, nInPackLen);
			break;
		case TRACKER_PROTO_CMD_SERVICE_QUERY_FETCH_ONE:
			result = tracker_deal_service_query_fetch_update( \
				&client_info, header.cmd, nInPackLen);
			break;
		case TRACKER_PROTO_CMD_SERVICE_QUERY_UPDATE:
			result = tracker_deal_service_query_fetch_update( \
				&client_info, header.cmd, nInPackLen);
			break;
		case TRACKER_PROTO_CMD_SERVICE_QUERY_FETCH_ALL:
			result = tracker_deal_service_query_fetch_update( \
				&client_info, header.cmd, nInPackLen);
			break;
		case TRACKER_PROTO_CMD_SERVICE_QUERY_STORE_WITHOUT_GROUP:
			result = tracker_deal_service_query_storage(
				&client_info, nInPackLen, header.cmd);
			break;
		case TRACKER_PROTO_CMD_SERVICE_QUERY_STORE_WITH_GROUP:
			result = tracker_deal_service_query_storage( \
				&client_info, nInPackLen, header.cmd);
			break;
		case TRACKER_PROTO_CMD_SERVER_LIST_GROUP:
			result = tracker_deal_server_list_groups( \
				&client_info, nInPackLen);
			break;
		case TRACKER_PROTO_CMD_SERVER_LIST_STORAGE:
			result = tracker_deal_server_list_group_storages( \
				&client_info, nInPackLen);
			break;
		case TRACKER_PROTO_CMD_STORAGE_SYNC_SRC_REQ:
			result = tracker_deal_storage_sync_src_req( \
				&client_info, nInPackLen);
			break;
		case TRACKER_PROTO_CMD_STORAGE_SYNC_DEST_REQ:
			result = tracker_deal_storage_sync_dest_req( \
				&client_info, nInPackLen);
			break;
		case TRACKER_PROTO_CMD_STORAGE_SYNC_NOTIFY:
			result = tracker_deal_storage_sync_notify( \
				&client_info, nInPackLen);
			break;
		case TRACKER_PROTO_CMD_STORAGE_SYNC_DEST_QUERY:
			result = tracker_deal_storage_sync_dest_query( \
				&client_info, nInPackLen);
			break;
		case TRACKER_PROTO_CMD_SERVER_DELETE_STORAGE:
			result = tracker_deal_server_delete_storage( \
				&client_info, nInPackLen);
			break;
		case FDFS_PROTO_CMD_QUIT:
			result = ECONNRESET;  //for quit loop
			break;
		default:
			logError("file: "__FILE__", line: %d, "  \
				"client ip: %s, unkown cmd: %d", \
				__LINE__, client_info.ip_addr, \
				header.cmd);
			result = EINVAL;
			break;
		}

		if (result != 0)
		{
			break;
		}

		count++;
	}

	if (g_continue_flag)
	{
		tracker_check_dirty(&client_info);
		tracker_mem_offline_store_server(&client_info);
	}

	if (client_info.pGroup != NULL)
	{
		--(*(client_info.pGroup->ref_count));
	}

	if (client_info.pStorage != NULL)
	{
		--(*(client_info.pStorage->ref_count));
	}

	close(client_info.sock);
	}

	if ((result=pthread_mutex_lock(&tracker_thread_lock)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"call pthread_mutex_lock fail, " \
			"errno: %d, error info: %s", \
			__LINE__, result, strerror(result));
	}
	g_tracker_thread_count--;
	if ((result=pthread_mutex_unlock(&tracker_thread_lock)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"call pthread_mutex_unlock fail, " \
			"errno: %d, error info: %s", \
			__LINE__, result, strerror(result));
	}

	return NULL;
}

