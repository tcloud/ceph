#include <unistd.h>
#include <sys/xattr.h>
#include <string.h>
#include <stdlib.h>
#include <iostream>
#include <vector>
#include <map>
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>

#if !defined(_countof)
	#define _countof(x) sizeof(x)/sizeof(*x)
#endif

enum {
	FQ_EXIT_SUCCESS 				= 0,
	FQ_EXIT_UNKNOW 				= 1,
	FQ_EXIT_ARGU					= 2,
	FQ_EXIT_SMALL_THAN_REAL		= 3,
	FQ_EXIT_SMALL_THAN_SUB			= 4,
	FQ_EXIT_TOO_LARGE				= 5,
	FQ_EXIT_COMMAND_NOT_FOUND		= 6,
};
const char *XATTR_FOLDER_QUOTA = "user.quota";
const char *XATTR_CEPH_RBYTES = "user.ceph.dir.rbytes";

bool is_folder(const std::string &sPath)
{
	bool bRet = true;
	struct stat st_buf;
	if (0==stat(sPath.c_str(),&st_buf)){
		if ( !S_ISDIR(st_buf.st_mode))
			bRet = false;;
	}else {
		bRet = false;
	}

	return bRet;
}

/*
 *   To find the nearest parent which has quota setting
 *   If found, update sParentPath & sParentSize
 *   return value:
 *   	true: found
 *   	false: not found or error occur
 */
bool find_parent_quota_entry(const std::string &sFolderPath,const std::string &sRootPath,
							std::string &sParentPath, std::string &sParentSize)
{
	std::string sPathTmp = sFolderPath;
	int iTmp = 0;
	sParentPath = sParentSize = "";
	bool bFound = false;

    while ( true ){
    	iTmp = sPathTmp.find_last_of("/", sPathTmp.length()-2);
    	if ( iTmp == (int) std::string::npos ){
    		break;
    	}
    	std::string sPathNew = sPathTmp.substr(0, iTmp);
    	if ( sPathNew == sRootPath || sPathTmp == sRootPath ){
    		break;
    	}

    	if ( !is_folder(sPathNew) ){
    		break;
    	}

    	char szTmp[1024] = {0};
    	if ( 0 < getxattr(sPathNew.c_str(), XATTR_FOLDER_QUOTA, szTmp, _countof(szTmp)) ){
    		std::cout << "Got entry path_new=" << sPathNew << " size=" << szTmp << std::endl;
    		sParentPath = sPathNew;
    		sParentSize = szTmp;
    		bFound = true;
    		break;
    	}else {
    		sPathTmp = sPathNew;
    	}
    }

    return bFound;
}

/*
 *  To sum the total quota setting of its sub-folders recursively
 *  The final result will be store in self._i_size_sum
 */
unsigned long long g_ullSizeSum = 0;
void sum_sub_quota(std::string sPathNow, const std::string &sPathStop)
{
	if ( !is_folder(sPathNow) )
		return;

	DIR *pdir;
	struct dirent *pdentry;
	if ( (pdir = opendir(sPathNow.c_str())) == NULL ) {
		return;
	}
	while (true) {
		if ((pdentry = readdir(pdir)) != NULL) {
			std::string sSubName = pdentry->d_name;
			std::string sSubPath = sPathNow + "/"+ sSubName;
			if ( is_folder(sSubPath) && sSubName!="." && sSubName!=".." ){
				if ( sSubPath != sPathStop ){
					char szTmp[64] = {0};
					if ( 0 < getxattr(sSubPath.c_str(), XATTR_FOLDER_QUOTA, szTmp, _countof(szTmp)) ){
						g_ullSizeSum += strtoull(szTmp, NULL, 10);
					}else {
						sum_sub_quota(sSubPath, sPathStop);
					}
				}
			}
		}else {
			break;
		}
	}
}

/*
 *  To get the quota setting of specified folder, and update to sSize
 *  return 	true: found
 *       	false: not found
 */
bool quota_get(const std::string &sFolderPath, std::string &sSize)
{
	bool bRet = false;
	if ( is_folder(sFolderPath) ){
		char szTmp[64] = {0};
    	if ( 0 < getxattr(sFolderPath.c_str(), XATTR_FOLDER_QUOTA, szTmp, _countof(szTmp)) ){
    		sSize = szTmp;
    		bRet = true;
    	}
	}
	return bRet;
}

std::map<std::string,std::string> g_mapQuotaList;
/*
 *   To get all quota setting of specified folder and its
 *   sub-folder recursively.
 *   The result will be store in g_mapQuotaList.
 *   The entry format in the list:
 *       {sPath, sSize}
 *   Only the entry with quota setting will be append to list.
 */
void quota_get_recu(std::string sFolderPath)
{
	if ( !is_folder(sFolderPath) ){
		return;
	}

	std::string sSize="";
	quota_get(sFolderPath, sSize);
	if ( sSize != "" ){
		g_mapQuotaList.insert( make_pair(sFolderPath, sSize) );
	}

	DIR *pdir;
	struct dirent *pdentry;
	if ( (pdir = opendir(sFolderPath.c_str())) == NULL ) {
		return;
	}
	while (true) {
		if ((pdentry = readdir(pdir)) != NULL) {
			std::string sSubName = pdentry->d_name;
			std::string sSubPath = sFolderPath + "/"+ sSubName;
			if ( is_folder(sSubPath) && sSubName!="." && sSubName!=".." ){
				quota_get_recu(sSubPath);
			}
		}else {
			break;
		}
	}
}

void usage()
{
	std::cout << "Usage: folder_quota Command Options" << std::endl;
	std::cout << "Command:" << std::endl;
	std::cout << "	set -p folder_path -r root_mount_path -s size" << std::endl;
	std::cout << "	unset -p folder_path" << std::endl;
	std::cout << "	unset_all -p folder_path" << std::endl;
	std::cout << "	list -p folder_path" << std::endl;
	std::cout << "	list_all -p folder_path" << std::endl;
	std::cout << std::endl;
}

//If error occurred, return -1
int option_parser(int argc, char ** argv, std::string &sFolderPath, std::string &sRootPath
		,std::string &sSize)
{
	int ch;
	opterr=0;
	int iRet = 0;
	while ( -1!=(ch=getopt(argc,argv,"p:r:s:")) ){
		switch(ch){
		case 'p':
			sFolderPath = optarg;
			break;
		case 'r':
			sRootPath = optarg;
			break;
		case 's':
			sSize = optarg;
			break;
		default:
			iRet = -1;
			break;
		}
	}
	return iRet;
}

/*
 *	Set quota for specified folder if following checks are sufficient.
 *       If the rbytes > the given size, return error
 *           ecode: ERROR_SMALL_THAN_REAL
 *       If the total quota size of sub-folders is larger than assigned size
 *           ecode: ERROR_SMALL_THAN_SUB
 *       If the paret's quota size is less than the total quota size of the parent's sub-folder
 *           ecode: ERROR_TOO_LARGE
 *
 */
int quota_set(std::string &rmsg, std::string &sFolderPath, std::string &sRootPath, std::string &sSize)
{
	int iRet = FQ_EXIT_SUCCESS;
    rmsg = "";
    char szCmd[1024] = {0};
    char szTmp[1024] = {0};
    //Check rbytes
    snprintf(szCmd, _countof(szCmd)-1, "touch %s", sFolderPath.c_str());
    system(szCmd);
    if ( 0 < getxattr(sFolderPath.c_str(), XATTR_FOLDER_QUOTA, szTmp, _countof(szTmp)) ){
    	if ( strtoull(szTmp, NULL, 10) > strtoull(sSize.c_str(), NULL, 10) ){
    		return FQ_EXIT_SMALL_THAN_REAL;
    	}
    }
    //todo: Checking: root_path must be part of folder_path
    //todo: handle symbol link
    std::string sParentPath="", sParentSize="";
    if ( find_parent_quota_entry(sFolderPath, sRootPath, sParentPath, sParentSize) ){
    	g_ullSizeSum = 0;
    	sum_sub_quota(sParentPath, sFolderPath);
    	if ( g_ullSizeSum + strtoull(sSize.c_str(), NULL, 10) > strtoull(sParentSize.c_str(), NULL, 10) ){
    		iRet = FQ_EXIT_TOO_LARGE;
    		rmsg = "EXIT_TOO_LARGE: Assign size is too large (free quota:" + sSize
					+ "). Parent(" + sParentPath + ") with size(" + sParentSize + ")";
    	}
    }

    if ( iRet == FQ_EXIT_SUCCESS){
    	g_ullSizeSum = 0;
    	sum_sub_quota(sFolderPath, sFolderPath);
    	if ( g_ullSizeSum > strtoull(sSize.c_str(),NULL, 10) ){
    		iRet = FQ_EXIT_SMALL_THAN_SUB;
    		snprintf(szTmp, _countof(szTmp)-1, "EXIT_SMALL_THAN_SUB: assigned size < sum of sub(%llu)",
    				g_ullSizeSum);
    		rmsg = szTmp;
    	}
    	else {
    		iRet = setxattr( sFolderPath.c_str(), XATTR_FOLDER_QUOTA, (void *)sSize.c_str(), sSize.length(), 0);
    		if ( iRet ){
    			snprintf(szTmp, _countof(szTmp)-1, "xattr_setxattr fail, iRet(%d), errno(%d)", iRet, errno);
    			rmsg = szTmp;
    			iRet = FQ_EXIT_UNKNOW;
    		}

    	}
    }

	return iRet;
}

/*
 * Clear quota setting for specified folder
 */
void quota_unset(const std::string &sFolderPath)
{
	std::string sSize;
	if ( quota_get(sFolderPath, sSize) ){
		removexattr(sFolderPath.c_str(), XATTR_FOLDER_QUOTA);
	}
}

/*
 * Clear all quota setting for specified folder and its sub-folder
 */
void quota_unset_all(const std::string &sFolderPath)
{
	if ( is_folder(sFolderPath) ){
		quota_unset(sFolderPath);

		DIR *pdir;
		struct dirent *pdentry;
		if ( (pdir = opendir(sFolderPath.c_str())) == NULL ) {
			return;
		}
		while (true) {
			if ((pdentry = readdir(pdir)) != NULL) {
				std::string sSubName = pdentry->d_name;
				std::string sSubPath = sFolderPath + "/"+ sSubName;
				if ( is_folder(sSubPath) && sSubName!="." && sSubName!=".." ){
					quota_unset_all(sSubPath);
				}
			}else {
				break;
			}
		}

	}
}

/*
 *   List quota setting for specified folder
 *   output format: {'path':'xxx', 'size:'xxx'}
 *       if has no quota setting, output nothing
 *
 */
int quota_list(const std::string &sFolderPath)
{
	std::string sSize;
	if ( quota_get(sFolderPath, sSize) ){
		std::cout << "{'path': '" << sFolderPath << "', 'size': '" << sSize << "'}" << std::endl;
	}

	return FQ_EXIT_SUCCESS;
}

/*
 *   List quota setting for specified folder and its sub-folder
 *   output example:
 *       {'path':'xxx', 'size:'xxx'}
 *       {'path':'xxx', 'size:'xxx'}
 *       {'path':'xxx', 'size:'xxx'}
 *
 */
int quota_list_all(const std::string &sFolderPath)
{
	g_mapQuotaList.clear();
	quota_get_recu(sFolderPath);
	for (std::map<std::string, std::string>::const_iterator it = g_mapQuotaList.begin()
			; it != g_mapQuotaList.end(); ++it){
		std::cout << "{'path': '" << it->first << "', 'size': '" << it->second << "'}" << std::endl;
	}
	return FQ_EXIT_SUCCESS;
}

int main (int argc, char ** argv)
{
	int iRet = FQ_EXIT_SUCCESS, iExit = FQ_EXIT_SUCCESS;
	std::string rmsg="";
	std::string sTmp;
	std::string sCommand="", sFolderPath="", sRootPath="", sSize="";

	try {
		if ( argc <= 2){
			std::cout << "Please specify Command to run" << std::endl;
			usage();
			throw (int)FQ_EXIT_ARGU;
		}
		sCommand = argv[1];
		iRet = option_parser(argc, argv, sFolderPath, sRootPath, sSize);
		if ( iRet != FQ_EXIT_SUCCESS ){
			throw (int)FQ_EXIT_ARGU;
		}
		if ( sCommand == "set" ){
			if (sFolderPath == "" || sRootPath == "" || sSize == ""){
				std::cout << "Option: need following options folder_path("<< sFolderPath <<
						"), root_path(" << sRootPath << "), size(" << sSize << ")" << std::endl;
			}
			iRet = quota_set(rmsg, sFolderPath, sRootPath, sSize);
			if ( iRet != FQ_EXIT_SUCCESS )
				throw iRet;
		}
		if ( sCommand == "unset" || sCommand == "unset_all" ||
				sCommand == "list" || sCommand == "list_all" ){
			if (sFolderPath == ""){
				std::cout << "Option: need following options folder_path("<< sFolderPath << ")" << std::endl;
				throw (int)FQ_EXIT_ARGU;
			}
			if ( sCommand == "unset" ) {
				quota_unset(sFolderPath);
			}
			else if (sCommand == "unset_all") {
				quota_unset_all(sFolderPath);
			}
			else if ( sCommand == "list" ) {
				iRet = quota_list(sFolderPath);
				if ( iRet != FQ_EXIT_SUCCESS )
					throw iRet;
			}
			else if ( sCommand == "list_all" ){
				iRet = quota_list_all(sFolderPath);
				if ( iRet != FQ_EXIT_SUCCESS )
					throw iRet;
			}
		}
	}
	catch(int iThrow){
		iExit = iThrow;
	}
	if ( iExit != FQ_EXIT_SUCCESS && rmsg != "" )
		std::cout << rmsg;

	return iExit;
}
