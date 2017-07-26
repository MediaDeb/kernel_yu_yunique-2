#include "tpd.h"
#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/time.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/gpio.h>
/* #define TIMER_DEBUG */


#include <linux/timer.h>


#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>

#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/device.h>


#include "tpd_custom_ft6x06.h"

#include <linux/module.h>

#include <linux/jiffies.h>

 extern void mt65xx_eint_unmask(unsigned int line);
 extern void mt65xx_eint_mask(unsigned int line);

#define ISP_FLASH_SIZE	0x8000 //32KB

#if defined(FT5436)
u8 is_5436_new_bootloader = 0;
u8 is_5436_fwsize_30 = 0;
#endif

/*==========================================*/
static tinno_ts_data *g_pts = NULL;

static int fts_i2c_read_a_byte_data(struct i2c_client *client, char reg)
{
	int ret;
	uint8_t iic_data;
	ret = i2c_smbus_read_i2c_block_data(client, reg, 1, &iic_data);
	if (iic_data < 0 || ret < 0){
		CTP_DBG("%s: i2c error, ret=%d\n", __func__, ret);
		return -1;
	}
	return (int)iic_data;
}

static inline int _lock(atomic_t *excl)
{
	if (atomic_inc_return(excl) == 1) {
		return 0;
	} else {
		atomic_dec(excl);
		return -1;
	}
}

static inline void _unlock(atomic_t *excl)
{
	atomic_dec(excl);
}

static ssize_t fts_isp_read(struct file *file, char __user *buf, size_t count, loff_t *offset)
{	
	//ret = copy_to_user(buf,&acc, sizeof(acc));
	CTP_DBG("");
	return -EIO;
}

static DECLARE_WAIT_QUEUE_HEAD(waiter_write);
static volatile	int write_flag;

 static int isp_thread(void *para)
 {
	int rc = 0;
	struct sched_param param = { .sched_priority = 4 };
	tinno_ts_data *ts = (tinno_ts_data *)para;
	sched_setscheduler(current, SCHED_RR, &param);
	set_current_state(TASK_RUNNING);	
	rc = fts_i2c_write_block(ts->isp_pBuffer, ts->isp_code_count);
	if (rc < ts->isp_code_count) {
		CTP_DBG("i2c_transfer failed(%d)", rc);
		ts->isp_code_count = -EAGAIN;
	} else{
		ts->isp_code_count = rc;
	}
	write_flag = 1;
	wake_up_interruptible(&waiter_write);
		
	return 0;
 }

static ssize_t fts_isp_write(struct file *file, const char __user *buf, size_t count, loff_t *offset)
{
	int rc = 0;
	tinno_ts_data *ts = file->private_data;
	char __user *start = buf;
	CTP_DBG("count = %d, offset=%d", count, (int)(*offset));
	
	if ( count > ISP_FLASH_SIZE ){
		CTP_DBG("isp code is too long.");
		return -EDOM;
	}

	if ( copy_from_user(ts->isp_pBuffer, start, count) ){
		CTP_DBG("copy failed(%d)", rc);
		return -EACCES;
	}

	ts->isp_code_count = count;
	write_flag = 0;
	ts->thread_isp= kthread_run(isp_thread, ts, TPD_DEVICE"-ISP");
	if (IS_ERR(ts->thread_isp)){ 
		rc = PTR_ERR(ts->thread_isp);
		CTP_DBG(" failed to create kernel thread: %d\n", rc);
		return rc;
	} 

	//block user thread
	wait_event_interruptible(waiter_write, write_flag!=0);
	
	return ts->isp_code_count;
}

static int fts_isp_open(struct inode *inode, struct file *file)
{
	CTP_DBG("try to open isp.");

	if ( atomic_read( &g_pts->ts_sleepState ) ){
		CTP_DBG("TP is in sleep state, please try again latter.");
		return -EAGAIN;
	}

	if (_lock(&g_pts->isp_opened)){
		CTP_DBG("isp is already opened.");
		return -EBUSY;
	}
		
	file->private_data = g_pts;

	g_pts->isp_pBuffer = (uint8_t *)kmalloc(ISP_FLASH_SIZE, GFP_KERNEL);
	if ( NULL == g_pts->isp_pBuffer ){
		_unlock ( &g_pts->isp_opened );
		CTP_DBG("no memory for isp.");
		return -ENOMEM;
	}
	
//	mt65xx_eint_mask(CUST_EINT_TOUCH_PANEL_NUM);
	
//	ft6x06_complete_unfinished_event();
	
#ifdef CONFIG_TOUCHSCREEN_FT5X05_DISABLE_KEY_WHEN_SLIDE
		fts_6x06_key_cancel();
#endif

	wake_lock(&g_pts->wake_lock);

	CTP_DBG("isp open success.");
	return 0;
}

static int fts_isp_close(struct inode *inode, struct file *file)
{
	tinno_ts_data *ts = file->private_data;
	
	CTP_DBG("try to close isp.");
	
	if ( !atomic_read( &g_pts->isp_opened ) ){
		CTP_DBG("no opened isp.");
		return -ENODEV;
	}
	
	kfree(ts->isp_pBuffer);
	ts->isp_pBuffer = NULL;
	
	file->private_data = NULL;
	
	_unlock ( &ts->isp_opened );
	
	wake_unlock(&ts->wake_lock);
	
//	mt65xx_eint_unmask(CUST_EINT_TOUCH_PANEL_NUM);  
	
	CTP_DBG("close isp success!");
	return 0;
}

int hid_to_i2c(tinno_ts_data *ts)
{
	uint8_t auc_i2c_write_buf[5] = {0};
	int bRet = 0;

	auc_i2c_write_buf[0] = 0xeb;
	auc_i2c_write_buf[1] = 0xaa;
	auc_i2c_write_buf[2] = 0x09;

	mutex_lock(&ts->mutex);
	i2c_master_send(ts->client, auc_i2c_write_buf, 3);
	mutex_unlock(&ts->mutex);

	msleep(10);

	auc_i2c_write_buf[0] = auc_i2c_write_buf[1] = auc_i2c_write_buf[2] = 0;

	ts->client->addr = ts->client->addr & I2C_MASK_FLAG;
	bRet = i2c_master_recv(ts->client, auc_i2c_write_buf, 3);
	if (bRet < 3) 
	{
		CTP_DBG("i2c_master_recv failed(%d)", bRet);
		return -EIO;
	} 

	if(0xeb==auc_i2c_write_buf[0] && 0xaa==auc_i2c_write_buf[1] && 0x08==auc_i2c_write_buf[2])
	{
		bRet = 1;		
	}
	else bRet = 0;

	return bRet;
	
}
static int fts_switch_to_update(tinno_ts_data *ts)
{
	int ret = 0, i=0;
	uint8_t arrCommand[] = {0x55, 0xaa};
	CTP_DBG("");
	
	/* hit to i2c */
	ret = hid_to_i2c(ts);

	if(ret == 0)
	{
		CTP_DBG("[FTS] hid change to i2c fail ! \n");
	}

#if defined(FT5446)	
	/*write 0xaa to register 0xfc*/
	ret = fts_write_reg(0xFC, 0xAA);
	if (ret < 0) {
		CTP_DBG("write 0xaa to register 0xbc failed");
		goto err;
	}
	//msleep(50);
	msleep(2);
	/*write 0x55 to register 0xfc*/
	ret = fts_write_reg(0xFC, 0x55);
	if (ret < 0) {
		CTP_DBG("write 0x55 to register 0xbc failed");
		goto err;
	}
	//msleep(40);
	msleep(200);
#else
    fts_6x06_hw_reset();
#endif 

	ret = hid_to_i2c(ts);
	if(ret == 0)
    	{
    		CTP_DBG("[FTS] hid change to i2c fail ! \n");
       }
   	msleep(10);
   	
	do
	{
		mutex_lock(&ts->mutex);
		ret = i2c_master_send(ts->client, (const char*)arrCommand, sizeof(arrCommand));
		mutex_unlock(&ts->mutex);
		++i;
	}while(ret < 0 && i < 5);

	ret = 0;
err:
	return ret;
}

static int fts_mode_switch(tinno_ts_data *ts, int iMode)
{
	int ret = 0;
	
	CTP_DBG("iMode=%d", iMode);
	
	if ( FTS_MODE_OPRATE == iMode ){
	}
	else if (FTS_MODE_UPDATE == iMode){
		ret = fts_switch_to_update(ts);
	}
	else if (FTS_MODE_SYSTEM == iMode){
	}
	else{
		CTP_DBG("unsupport mode %d", iMode);
	}
	return ret;
}


//BEGIN <add changing flag> <DATE20130330> <add changing flag> zhangxiaofei
int fts_ft6x06_switch_charger_status(u8 charger_flag)
{
    int ret = 0;
    u8 vl_read_charger_flag = 0;

    CTP_DBG("charger_flag =  %d\n",charger_flag);


    ret = fts_write_reg(0x8b, charger_flag);
    if (ret < 0) {
        CTP_DBG("write  charger_flag(%d) to register 0x8b failed", charger_flag);
        goto err;
    }
    msleep(50);

// read check
#if 0
    ret = fts_read_reg(0x8b, &vl_read_charger_flag);
    if (ret < 0){
        CTP_DBG("read  0x8b failed");
        goto err;
    }
    
    CTP_DBG("read  vl_read_charger_flag = %d, from register 0x8b", vl_read_charger_flag);
#endif    

err:
	return ret;
}
//END <add changing flag> <DATE20130330> <add changing flag> zhangxiaofei


static int fts_ctpm_auto_clb(void)
{
    unsigned char uc_temp[1];
    unsigned char i ;

    CTP_DBG("start auto CLB.\n");
    msleep(200);
    fts_write_reg(0, 0x40);  
    mdelay(100);   //make sure already enter factory mode
    fts_write_reg(2, 0x4);  //write command to start calibration
    mdelay(300);
    for(i=0;i<100;i++)
    {
        fts_read_reg(0,uc_temp);
        if ( ((uc_temp[0]&0x70)>>4) == 0x0)  //return to normal mode, calibration finish
        {
            break;
        }
        mdelay(200);
        CTP_DBG("waiting calibration %d\n",i);
        
    }
    CTP_DBG("calibration OK.\n");
    
    msleep(300);
    fts_write_reg(0, 0x40);  //goto factory mode
    mdelay(100);   //make sure already enter factory mode
    fts_write_reg(2, 0x5);  //store CLB result
    mdelay(300);
    fts_write_reg(0, 0x0); //return to normal mode 
    msleep(300);
    CTP_DBG("store CLB result OK.\n");
    return 0;
}

static int ft6x06_get_tp_id(tinno_ts_data *ts, int *ptp_id)
{
	int rc;
	char tp_id[2];
	*ptp_id = -1;
	
	CTP_DBG("Try to get TPID!");
	
	rc = fts_cmd_write(0x90, 0x00, 0x00, 0x00, 4);
	if (rc < 4) {
		CTP_DBG("i2c_master_send failed(%d)", rc);
		return -EIO;
	} 
	
	ts->client->addr = ts->client->addr & I2C_MASK_FLAG;
	rc = i2c_master_recv(ts->client, tp_id, 2);
	if (rc < 2) {
		CTP_DBG("i2c_master_recv failed(%d)", rc);
		return -EIO;
	} 

	*ptp_id = (( int )tp_id[0] << 8) | (( int )tp_id[1]);
			
	return 0;
}

static int ft6x06_get_fw_version(tinno_ts_data *ts)
{
	int ret;
	uint8_t fw_version;
	ret = fts_read_reg(0xA6, &fw_version);
	if (ret < 0){
		CTP_DBG("i2c error, ret=%d\n", ret);
		return -1;
	}
	CTP_DBG("fw_version=0x%X\n", fw_version);
	return (int)fw_version;
}

int ft6x06_get_ic_status(void)
{
	int ret;
	uint8_t ic_status;
	ret = fts_read_reg(0x00, &ic_status);
	if (ret < 0){
		CTP_DBG("i2c error, ret=%d\n", ret);
		return -1;
	}
	CTP_DBG("ic_status=0x%X\n", ic_status);
	return (int)0;
}
int get_fw_version_ext(void)
{
	int version = -1;
	if(g_pts)
		version = ft6x06_get_fw_version(g_pts);
	CTP_DBG("fw_version=0x%X\n", version);
	return version;
}
EXPORT_SYMBOL(get_fw_version_ext);

static u8 ft6x06_get_bootloader_version(void)
{
    int rc = 0;
    u8 version_id = 0;
    //get bootloader version
    rc = fts_cmd_write(0xcd, 0x00, 0x00, 0x00, 1);
    if (rc < 1) {
        CTP_DBG("i2c_master_send failed(%d)", rc);
        return 0;
    } 
    rc = i2c_master_recv(g_pts->client, &version_id, 1);
    if (rc < 1) {
        CTP_DBG("i2c_master_recv failed(%d)", rc);
        return 0;
    } 
    CTP_DBG("bootloader version = 0x%x\n", version_id);
    return version_id;

}

static int ft6x06_get_vendor_from_bootloader(tinno_ts_data *ts, uint8_t *pfw_vendor, uint8_t *pfw_version)
{
	int rc = 0, tp_id;
	uint8_t version_id = 0, buf[5];
	CTP_DBG();
	CTP_DBG("Try to switch to update mode!");
	
	rc = fts_mode_switch(ts, FTS_MODE_UPDATE);
	if(rc)
	{
		CTP_DBG("switch to update mode error");
		goto err;
	}
	
	rc = ft6x06_get_tp_id(ts, &tp_id);
	CTP_DBG("TP ID=0x%X!", tp_id);
	if(rc)
	{
		CTP_DBG("Get tp ID error(%d)", rc);
		goto err;
	}
	if ( FTS_CTP_FIRWARE_ID != tp_id ){
		CTP_DBG("Tp ID is error(0x%x)", tp_id);
		rc = -EINVAL;
		goto err;
	}

	//Get bootloader version
	rc = fts_cmd_write(0xcd, 0x00, 0x00, 0x00, 1);
	if (rc < 1) {
		CTP_DBG("i2c_master_send failed(%d)", rc);
		goto err;
	} 
	rc = i2c_master_recv(ts->client, &version_id, 1);
	if (rc < 1) {
		CTP_DBG("i2c_master_recv failed(%d)", rc);
		goto err;
	} 
	
	//*pfw_version = version_id;
	*pfw_version = -1;//Force to update.
	CTP_DBG("bootloader version = 0x%x\n", version_id);

	/* --------- read current project setting  ---------- */
	//set read start address
	#if defined(FT5446)
	//rc = fts_cmd_write(0x03, 0x00, 0x07, 0xb0, 4);
	rc = fts_cmd_write(0x03, 0x00, 0xd7, 0x80, 4);
	#else
	rc = fts_cmd_write(0x03, 0x00, 0x78, 0x00, 4);
	
	#endif
	if (rc < 4) {
		CTP_DBG("i2c_master_send failed(%d)", rc);
		goto err;
	} 
	
	rc = i2c_master_recv(g_pts->client, buf, sizeof(buf));
	if (rc < 0){
		CTP_DBG("i2c_master_recv failed(%d)", rc);
		goto err;
	}

	CTP_DBG("bootloader vendor_id = 0x%x\n", buf[4]);
	
	*pfw_vendor = buf[4];
	
	CTP_DBG("Try to reset TP!");
	rc = fts_cmd_write(0x07,0x00,0x00,0x00,1);
	if (rc < 0) {
		CTP_DBG("reset failed");
		goto err;
	}
	msleep(200);
	
	return 0;
err:
	return rc;
}

int ft6x06_get_vendor_version(tinno_ts_data *ts, uint8_t *pfw_vendor, uint8_t *pfw_version)
{
	int ret;
	*pfw_version = ft6x06_get_fw_version(ts);

	ret = fts_read_reg(0xA8, pfw_vendor);
	if (ret < 0){
		CTP_DBG("i2c error, ret=%d\n", ret);
		return ret;
	}
	CTP_DBG("one fw_vendor=0x%X, fw_version=0x%X\n", *pfw_vendor, *pfw_version);
	if ( 0xA8 == *pfw_vendor || 0x00 == *pfw_vendor ){
		CTP_DBG("FW in TP has problem, get factory ID from bootloader.\n");
		ret = ft6x06_get_vendor_from_bootloader(ts, pfw_vendor, pfw_version);
		if (ret){
			CTP_DBG("ft6x06_get_vendor_from_bootloader error, ret=%d\n", ret);
			return -EFAULT;
		}
	}
	
	CTP_DBG("two fw_vendor=0x%X, fw_version=0x%X\n", *pfw_vendor, *pfw_version);
	return 0;
}
EXPORT_SYMBOL(ft6x06_get_vendor_version);

extern void ft6x06_tp_upgrade(const char * ftbin_buf, int buf_len);
int ft6x06_fw_upgrade_from_file(void)
{
    int ret = -1;

    printk("[ft6x06]  Entry ft6x06_fw_upgrade_from_file \n");
	
   // ft6x06_tp_upgrade(ft6x06_file_fw_data,ft6x06_file_fw_data_len);

    return 0;
}

static int fts_isp_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	tinno_ts_data *ts = file->private_data;
	int flag;
	int rc = 0;
	
	if ( !atomic_read( &g_pts->isp_opened ) ){
		CTP_DBG("no opened isp.");
		return -ENODEV;
	}
	
	/* check cmd */
	if(_IOC_TYPE(cmd) != FT6x06_IOCTLID)	
	{
		CTP_DBG("cmd magic type error");
		return -EINVAL;
	}
	
	if(_IOC_DIR(cmd) & _IOC_READ)
		rc = !access_ok(VERIFY_WRITE,(void __user*)arg, _IOC_SIZE(cmd));
	else if(_IOC_DIR(cmd) & _IOC_WRITE)
		rc = !access_ok(VERIFY_READ, (void __user*)arg, _IOC_SIZE(cmd));
	if(rc)
	{
		CTP_DBG("cmd access_ok error");
		return -EINVAL;
	}

	switch (cmd) {
        case FT6x06_IOCTL_FW_UPDATE:
            printk("[ft6x06] tp upgrade: , ft6x06_file_fw_data_len = %d\n",ft6x06_file_fw_data_len);
            if((ft6x06_file_fw_data == NULL) || (ft6x06_file_fw_data_len <= 0))
            {
                return 0;
            }
            else
            {
                int ret = 0;
                ret = ft6x06_fw_upgrade_from_file();
                printk("[ft6x06]  ft6x06_fw_upgrade_from_file ret =%x ", ret);
                ft6x06_file_fw_data = NULL;
                ft6x06_file_fw_data_len = 0;
                return ret;
            }
            break;

        case FT6x06_IOCTL_TP_UPGRADE_SET_BIN_BUF:
            ft6x06_file_fw_data = (uint8_t *)arg;
            break;

        case FT6x06_IOCTL_TP_UPGRADE_SET_BIN_LEN:
            ft6x06_file_fw_data_len = (int)arg;
            break;
			
		case FT5X06_IOCTL_GET_VENDOR_VERSION:
		{
			int rc;
			uint8_t vendor = 0;
			uint8_t version = 0;
			CTP_DBG("Try to get vendor_version!");
			rc = ft6x06_get_vendor_version(ts, &vendor, &version);
			if ( rc < 0 ){
			printk("AAAA\n");	
			rc = -EFAULT;
			break;
			}
			printk("vendor version =%d, version=%d!", vendor, version);
			flag = (vendor<<8)|version;
			if(copy_to_user(argp,&flag, sizeof(int))!=0)
			{
			CTP_DBG("copy_to_user error");
			rc = -EFAULT;
			}
			break;
		}	
        default:
            break;
    }

	return rc;
}

#if defined(FTS_AUTO_TP_UPGRADE)
#ifdef TPD_FACTORY_FW_UPDATE 
extern bool ftm_ft6x06_force_update;
#endif
void ft6x06_tp_upgrade(const char * ftbin_buf, int buf_len)
{
        int vl_tp_ver = 0x00;
        int vl_bin_ver = 0x00;
        int rc = 0;
        int i = 0;
        int tp_id = 0;
        char reg_val[2] = {0};
        u8 auc_i2c_write_buf[5] = {0};
        
#if defined(FT5436)
        U8 bootloader_version = 0;
#endif
        vl_bin_ver = ftbin_buf[buf_len-2];
        vl_tp_ver = get_fw_version_ext();
		#ifdef TPD_FACTORY_FW_UPDATE 
        CTP_DBG("bin_ver= 0x%x, current_tp_ver= 0x%x,force:%d", vl_bin_ver, vl_tp_ver,ftm_ft6x06_force_update);
        if((vl_bin_ver > vl_tp_ver) || (ftm_ft6x06_force_update == true))
		#else
		CTP_DBG("bin_ver= 0x%x, current_tp_ver= 0x%x", vl_bin_ver, vl_tp_ver);
        if(vl_bin_ver > vl_tp_ver)
		#endif
        {
            CTP_DBG(" have new ver, upgrade... \n");
            CTP_DBG("Step 1:switch mode from WORK to UPDATE...");

#if defined(FT5436)
            if(ftbin_buf[buf_len-12] == 0x1e) // 30
	        {    
	            is_5436_fwsize_30 = 1;
	        }
	        else 
	        {
		        is_5436_fwsize_30 = 0;
	        }
#endif
	     
            rc = fts_mode_switch(g_pts, (int)FTS_MODE_UPDATE);
            if(rc)
            {
                CTP_DBG("switch to update mode error");
                return; //  -EIO;
            }
           // msleep(10);
            msleep(1);
            CTP_DBG("Step 2:check READ-ID...");
            for( i = 0; i < 3; i++ )
            {
                rc = ft6x06_get_tp_id(g_pts, &tp_id);
                CTP_DBG("TPID=0x%X!", tp_id);
                if(rc)
                {
                    CTP_DBG("Get tp ID error(%d)", rc);
                    return ; // rc = -EIO;
                } 
                else
                {
                    CTP_DBG("tp_id=0x%X\n", tp_id);
                    // 5316-->7907      5206-->7903     5446-->542c
                    if ( FTS_CTP_FIRWARE_ID == tp_id ){
                        CTP_DBG("check id OK \n");
                        break;
                    }
                }
            }
            if ( i == 3 )
            {
                CTP_DBG("\n CHECK-ID error!");
                return; // goto err_read_id;
            }

            /*********get  bootloader ********************/
           #if defined(FT5436)
            bootloader_version = ft6x06_get_bootloader_version();
	        if (bootloader_version <= 4)
	        {
	            is_5436_new_bootloader = BL_VERSION_LZ4 ;
	        }
	        else if(bootloader_version == 7)
	        {
		       is_5436_new_bootloader = BL_VERSION_Z7 ;
	        }
	        else if(bootloader_version >= 0x0f)
	        {
		        is_5436_new_bootloader = BL_VERSION_GZF ;
	        }
            #endif
            
            /*********Step 4:erase app and panel paramenter area ********************/
            CTP_DBG("Step 4:erase falsh...");
#if defined(FT5436)
	        if(is_5436_fwsize_30)
	        {
	             CTP_DBG(" erase falsh 0x61 0x63 \n");
                 rc = fts_cmd_write(0x61, 0, 0, 0, 1);
                 if (rc < 1) {
                     CTP_DBG("erase failed");
                     return;
                 }
                 msleep(FT_UPGRADE_EARSE_DELAY); 
		         rc = fts_cmd_write(0x63, 0, 0, 0, 1);
                 if (rc < 1) {
                    CTP_DBG("erase failed");
                 return;
                 }
                msleep(FT_UPGRADE_EARSE_DELAY); 
	       }
	       else
	       {
	           rc = fts_cmd_write(0x61, 0, 0, 0, 1);
               if (rc < 1) {
                   CTP_DBG("erase failed");
                    return;
               }
               msleep(FT_UPGRADE_EARSE_DELAY); 
	       }
#else
           rc = fts_cmd_write(0x61, 0, 0, 0, 1);
           if (rc < 1) {
                CTP_DBG("erase failed");
                return;
            }
            msleep(FT_UPGRADE_EARSE_DELAY);
#endif

#if defined(FT5446)
            for(i = 0;i < 15;i++)
    	     {
               rc = fts_cmd_write(0x6a, 0, 0, 0, 1);
               if (rc < 1) 
               {
                    CTP_DBG("erase failed");
                    return;
                }
    		 reg_val[0] = reg_val[1] = 0x00;
               rc = i2c_master_recv(g_pts->client, reg_val, 2);
    		if(0xF0==reg_val[0] && 0xAA==reg_val[1])
    		{
        	      break;
    		}
    		msleep(50);
           }
#endif  //defined(FT5446)

            CTP_DBG("Step 6:write firmware(FW) to ctpm flash");

            #if defined(FT5436)
            if(is_5436_new_bootloader == BL_VERSION_LZ4 || is_5436_new_bootloader == BL_VERSION_Z7 )
            {
                buf_len = buf_len - 8;
            }else if(is_5436_new_bootloader == BL_VERSION_GZF)
            {
                buf_len = buf_len - 14;
            }
            #endif  
            
            /*write the length of the fw */
            auc_i2c_write_buf[0] = 0xB0;
            auc_i2c_write_buf[1] = (u8) ((buf_len >> 16) & 0xFF);
            auc_i2c_write_buf[2] = (u8) ((buf_len >> 8) & 0xFF);
            auc_i2c_write_buf[3] = (u8) (buf_len & 0xFF); 
            fts_cmd_write(auc_i2c_write_buf[0], auc_i2c_write_buf[1], auc_i2c_write_buf[2], auc_i2c_write_buf[3], 4);
           
            rc = fts_i2c_write_block(ftbin_buf, buf_len);
            msleep(100); 
            if ( rc != buf_len ){
                CTP_DBG("\n Error,  write rc (%d) != buf_len(%d) !!!!!!!!!", rc , buf_len);
                      msleep(5); 
                return;
            }
            else {
            CTP_DBG("write OK \n");
            }
		#if 0
            CTP_DBG("Step 9:read out checksum...");
                uint8_t check_sum;
                CTP_DBG("Try to get checksum!");
                fts_cmd_write(0xCC,0x00,0x00,0x00,1);
                ts->client->addr = ts->client->addr & I2C_MASK_FLAG;
                rc = i2c_master_recv(ts->client, &check_sum, 1);
                if (rc < 0) {
                    CTP_DBG("read checksum failed");
                }
                CTP_DBG("checksum=%d!", check_sum);
                flag = check_sum;
                if(copy_to_user(argp,&flag, sizeof(int))!=0)
                {
                    CTP_DBG("copy_to_user error");
                    return;//rc = -EFAULT;
                }
		#endif
        // same as hw reset, tp comand reset sometime do not work
		#if 1
            CTP_DBG("Step 10:reset the new FW...");
    
            rc = fts_cmd_write(0x07,0x00,0x00,0x00,1);
            if (rc < 0) {
                CTP_DBG("reset failed");
                return;
            }
    
               msleep(200);  //make sure CTP startup normally
	    #endif
    
            // 6x06 6x08 do not cal
            //CTP_DBG("Step 11: Calibrate the TP, please don't touch the TP before the operation is finished! ...");
            //fts_ctpm_auto_clb();
           // fts_6x06_hw_reset();
    
        }
        else
        {
            CTP_DBG("tp firmware needn't upgrade. \n");
        }
        
}
#endif

static const struct file_operations fts_isp_fops = {
	.owner = THIS_MODULE,
	.read = fts_isp_read,
	.write = fts_isp_write,
	.open = fts_isp_open,
	.release = fts_isp_close,
	.unlocked_ioctl = fts_isp_ioctl,
};

static struct miscdevice fts_isp_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "fts_isp",
	.fops = &fts_isp_fops,
};

 /* called when loaded into kernel */
 int fts_6x06_isp_init( tinno_ts_data *ts ) 
 {
 	int ret;
	CTP_DBG("MediaTek FT6x06 touch panel isp init\n");
	
	wake_lock_init(&ts->wake_lock, WAKE_LOCK_SUSPEND, "fts_tp_isp");
	
	ret = misc_register(&fts_isp_device);
	if (ret) {
		misc_deregister(&fts_isp_device);
		printk(KERN_ERR "fts_isp_device device register failed (%d)\n", ret);
		goto exit_misc_device_register_failed;
	}
	
	g_pts = ts;
	return 0;
	
exit_misc_device_register_failed:
	misc_deregister(&fts_isp_device);
	fts_isp_device.minor = MISC_DYNAMIC_MINOR;
	wake_lock_destroy(&ts->wake_lock);
	return ret;
 }
 
 /* should never be called */
 void fts_6x06_isp_exit(void) 
{
	CTP_DBG("MediaTek FT6x06 touch panel isp exit\n");
	if ( g_pts ){
		misc_deregister(&fts_isp_device);
		fts_isp_device.minor = MISC_DYNAMIC_MINOR;
		wake_lock_destroy(&g_pts->wake_lock);
		g_pts = NULL;
	}
}
 
