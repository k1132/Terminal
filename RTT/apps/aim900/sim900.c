#include    <string.h>
#include    <stdlib.h>
#include    <stdio.h>
#include    "sim900.h"
#include    "ringbuffer.h"
#include    "gsm_driver.h"
#include 	"strLib.h"
#include    <rtthread.h>
	   

#if ( SIM900_EN > 0 )
/*private sem locker*/
static struct rt_semaphore  sim900_lock;    //lock for sim900 OP
#define SIM900_LOCK()		rt_sem_take(&sim900_lock,RT_WAITING_FOREVER)
#define SIM900_UNLOCK()		rt_sem_release(&sim900_lock)


/*ring buffer for IP DATA*/
static  rt_uint8_t* IPdata_buff;
static  struct rb   IPdata_rb;
static struct rt_semaphore  rb_lock;        //lock for ipdata rb OP
#define RINGBUFFER_LOCK()	rt_sem_take(&rb_lock,RT_WAITING_FOREVER)
#define RINGBUFFER_UNLOCK()	rt_sem_release(&rb_lock)

/*default ip & port*/
static rt_uint8_t   default_remoteIPADDR1[] = "117.25.148.45";
static rt_uint8_t   default_remotePORT1[]   = "1553";
static rt_uint8_t   default_connTYPE1[]     = "TCP";

/*default phone number*/
static rt_uint8_t   default_PhoneNumber[]   = "13512341234";


/*sim900 device*/
static  rt_sim900_device*   sim900;

rt_size_t rt_sim900_write(const void* buffer, rt_size_t size);
void sim900_set_attribute(rt_uint8_t** attribute, rt_uint8_t* content);
//<CR><LF><response><CR><LF>
// /r  /n  response  /r /n
// 0D  0A  response  0D 0A


static rt_uint8_t GetCommLineStatus(void)
{
    return sim900->CommLineStatus;
}

static void SetCommLineStatus(rt_uint8_t status)
{
    sim900->CommLineStatus = status;
}

//AT response are send to msg queue
//call by rt_thread_sim900_rx_entry
//parse "RING","CLOSED" response and send event seperately
//return err code
static rt_err_t SendATResp(rt_uint8_t* at_resp_msg)
{
    rt_err_t ret = RT_ERROR;;

    RT_ASSERT(&sim900->AT_resp_MQ != RT_NULL);

    if(rt_strncmp( (char*)(&at_resp_msg[0]), "CLOSED", 6)==0)
    {
    	ret = rt_event_send(&sim900->ATResp_event, CLOSED);
    }
    else if(rt_strncmp( (char*)(&at_resp_msg[0]), "RING", 4)==0)
    {
        if(GetCommLineStatus() != IN_CALL)
		{
        	ret = rt_event_send(&sim900->ATResp_event, RING);
		}
    }
    else
	{
        ret = rt_mq_send(&sim900->AT_resp_MQ, at_resp_msg, rt_strlen((char*)&at_resp_msg[0]) + 1);
    }

    if(ret == -RT_EFULL)
        rt_kprintf("AT response MQ is full\r\n");

    return ret;
}

//AT response in msg queue are captured by thread call this function
//must call by a thread
//will block thread for max_tout
//return length of response string
static rt_int8_t WaitATResp(rt_uint8_t* get_resp_str, rt_int32_t max_tout )
{
    // buffer for msg recv from MQ less than SIM900_AT_RESP_MQ_MSG_SIZE:32
	rt_uint8_t	ATResp_buff[SIM900_AT_RESP_MQ_MSG_SIZE];
    rt_int8_t	i = 0;     								
    rt_err_t	ret;

    rt_memset(ATResp_buff, 0, SIM900_AT_RESP_MQ_MSG_SIZE);		  

    //read at response msg queue
    RT_ASSERT(&sim900->AT_resp_MQ != RT_NULL);
    ret = rt_mq_recv(&sim900->AT_resp_MQ, ATResp_buff, SIM900_AT_RESP_MQ_MSG_SIZE,  max_tout);

    if( ret == RT_EOK )
    {
        //get string length
		i = rt_strlen((char*)ATResp_buff);
        rt_strncpy((char*)get_resp_str, (char*)ATResp_buff, i);
    }
    else
    {
        rt_kprintf("AT response recieve failed\r\n");
        i = -1;
    }

    return i;
}

//simple function: wrap the event op for ipdata recvieved
//CALL BY RX thread
//when a frame data recieved
//send event
static rt_err_t ipdata_recieved(void)
{
    return rt_sem_release(&sim900->frame_sem);
}

//call by IP DATA read funtion
//block and wait for one frame data arrive
static rt_uint8_t ipdata_arrived(void)
{
    if(rt_sem_take(&sim900->frame_sem, RT_WAITING_FOREVER) == RT_EOK)
	{
    	return 1;
	}
    else
	{
    	return 0;
	}
}


/****************************************
*   rt_thread_sim900_rx_entry           *
*   bottom op of sim900 read            *
*                                       *
*   (1) handle rx byte                  *
*   (2) modem mode:read at respnse      *
*       send AT event to proc thread    *
*   (3)	ppp mode:fill ip data into buf  *
*       send sem to sio_read            *
*                                       *
****************************************/
rt_uint8_t* ATRes_buff;
static void rt_thread_sim900_rx_entry(void* parameter)
{
    rt_uint8_t  i = 0;
    rt_uint8_t  j = 0;
    rt_uint8_t  thischar = 0;
    rt_uint8_t  ch[5];

    rt_uint8_t  recv_type = recv_ats;
    
    rt_uint8_t  bufSz = SIM900_AT_RESP_MQ_MSG_SIZE;
    rt_uint16_t ip_data_len = 0;

    ATRes_buff = (rt_uint8_t*)rt_malloc_align(bufSz, RT_ALIGN_SIZE);
    rt_memset(&ATRes_buff[0], 0, bufSz);
    rt_memset(&ch[0], 0, sizeof(&ch[0]));

    while(1)
    {
        if(rt_sem_take(&sim900->rx_semaphore, RT_WAITING_FOREVER) != RT_EOK)
		{
            continue;
		}

        while(rt_device_read(sim900->device, 0, &thischar, 1) == 1)
        {
			// <CR><LF>RESPONSE<CR><LF> //0d 0a 4F 4B 0d 0a
            switch(recv_type)
            {
                case recv_ats:
                {	
					//recieve at
                    ch[4] = ch[3];
                    ch[3] = ch[2];
                    ch[2] = ch[1];
                    ch[1] = ch[0];
                    ch[0] = thischar;
					if(i < bufSz)
					{
                        ATRes_buff[i++] = thischar;		
					}

                    /* check CR key */
                    if((ch[1] == 0x0D) && (ch[0] == 0x0A))
                    {
						if((ATRes_buff[0] == 0x0D) && (ATRes_buff[1] == 0x0A) && (i>2))
						{				
							//send at cmd response
							del_char((char *)ATRes_buff, 0x0D);
							del_char((char *)ATRes_buff, 0x0A);
							SendATResp(&ATRes_buff[0]);
							RT_DEBUG_LOG(DEBUG_SIM900, ("<----- %s\n", ATRes_buff));  
						}

						if(i != 2)
						{
							rt_memset(&ATRes_buff[0], 0, bufSz);
		                    i = 0;
						}						
                    }

                    /*check IP data, using IP head before ip package */
                    else if((ch[4] == '+') && (ch[3] == 'I') && (ch[2] == 'P') && (ch[1] == 'D') && (ch[0] == ','))
                    {
						// switch to handle ip data"+IPD,"
                        recv_type = recv_ipdata_len;
                        rt_memset(&ch[0], 0, 4);
                        rt_memset(&ATRes_buff[0], 0, bufSz);
                        i = 0;
                        SetCommLineStatus(IN_TCPUDP_NONTRANSP);
                    }
                    else if(thischar == '>')
                    {
                        SendATResp(&thischar);
                        rt_memset(&ATRes_buff[0], 0, bufSz);
                        i = 0;
                    }
                }
                break;

                case recv_ipdata_len:
                {   
					//(1)get data length
                    if(thischar != ':')
                    {
						if(thischar >= '0'&& thischar <= '9')
						{
							ip_data_len = ip_data_len*10 + (thischar-'0');
						}
						else
						{
							rt_kprintf("ip data len error\r\n");
						}
                    }
                    else
                    {
                        rt_kprintf("ip data len %d \r\n", ip_data_len);
                        recv_type = recv_ipdata;
                        j = 0;
                    }
                }
                break;

                case recv_ipdata:
                {   
					//(2)get ip data
                    if(j < ip_data_len)
                    {
                        RINGBUFFER_LOCK();
                        rb_put(&IPdata_rb, &thischar, 1);
                        RINGBUFFER_UNLOCK();
                        j++;
                    }
                    else
                    {   
						//get data accomplished
                    	SetCommLineStatus(STANDBY);
                        recv_type = recv_ats;
                        rt_kprintf("ip data recieved\r\n");
                        ipdata_recieved();
                    }
                }
                default:
                break;
            }
        }
    }
}


/****************************************
*   rt_sim900_thread_startup            *
*   startup the sim900 reset entry      *
*   normal startup procedure:           *
*   1. rt_sim900_hw_init                *
*   2. rt_sim900_thread_startup         *
****************************************/
void rt_sim900_thread_startup(void)
{
    rt_thread_t tid;
    tid= rt_thread_create(  "s900_rst",
                            &rt_thread_sim900_rx_entry,
                            RT_NULL,
                            512, 2, 10 );
    if(tid != RT_NULL)
        rt_thread_startup(tid);
}


/********************************************
* This function read ipdata from sim900     *
* block type, use it in app thread          *
* paras: ip data buffer pointer             *
* @return the byte number of sim900 read    *
********************************************/
rt_size_t sim900_readDATA(void* buffer)
{
    rt_size_t   read_len;
    rt_uint8_t  i = 0;
    rt_uint8_t* buf = (rt_uint8_t*)buffer;

    /*wait for ipdata recv*/
    rt_memset(&buf[0], 0, sizeof(&buf[0]));
    if(ipdata_arrived())
    {	
		/*read all byte in ring buffer*/
        RINGBUFFER_LOCK();
        while(rb_get(&IPdata_rb, &buf[i], 1) == RT_TRUE)
		{
            i++;
		}
        RINGBUFFER_UNLOCK();
        /*read done*/
    }

    read_len = i;
    return read_len;
}


/********************************************
* This function send ipdata to sim900       *
* non transparent mode                      *
* @return the byte number of sim900 sent.   *
********************************************/
rt_size_t sim900_sendDATA(const void* buffer, rt_size_t size)
{
    rt_size_t   write_len = 0;
    rt_uint8_t  CMDresp[SIM900_AT_RESP_MQ_MSG_SIZE];
    rt_uint8_t  TCPsendcmd[SIM900_AT_RESP_MQ_MSG_SIZE] = "AT+CIPSEND\r\n";//len=12


    /*send ip data pack*/
    rt_sim900_write(TCPsendcmd, rt_strlen((char*)TCPsendcmd));
    rt_memset(&CMDresp[0], 0, SIM900_AT_RESP_MQ_MSG_SIZE);
    if(WaitATResp(&CMDresp[0], RT_TICK_PER_SECOND) > 0)
    {
        if(rt_strncmp( (char*)(&CMDresp[0]), ">", 1) == 0)//wait for PROMOT_MARK
        {
        	SetCommLineStatus(IN_TCPUDP_NONTRANSP);
            rt_sim900_write(buffer, size);
            rt_sim900_write("\1A", 1);
        }
    }

    rt_memset(&CMDresp[0], 0, SIM900_AT_RESP_MQ_MSG_SIZE);
    if(WaitATResp(&CMDresp[0], 5*RT_TICK_PER_SECOND) > 0)
    {	
		//wait for "send ok"
        if(rt_strncmp( (char*)(&CMDresp[0]), "SEND OK", 7) == 0)
        {
        	SetCommLineStatus(IN_TCPUDP_NONTRANSP);
            return write_len;
        }
        else if(rt_strncmp( (char*)(&CMDresp[0]), "SEND FAIL", 9) == 0)
        {
            SetCommLineStatus(IN_TCPUDP_NONTRANSP);
            return 0;
        }
    }
    return 0;
}


/****************************************
*   sim900 call functions               *
*   Call outgoing operation             *
*   return RT_ERROR / RT_EOK            *
****************************************/
static rt_err_t sim900_Call(rt_uint8_t* number_str)
{
    rt_uint8_t  i=0;
    rt_uint8_t  phone_number_len = 0;
    rt_uint8_t  number[16];
    char str[SIM900_AT_RESP_MQ_MSG_SIZE];
    rt_uint8_t  CMDresp[SIM900_AT_RESP_MQ_MSG_SIZE];


    /* comm line is ARLREADY IN CALL */
    if( GetCommLineStatus() == IN_CALL)
        return RT_ERROR;

  	//set comm line occupied
    SetCommLineStatus(IN_CALL);
    rt_memset(str, 0, sizeof(str));
    if(number_str != RT_NULL)
    {	
		//use input phone number
        phone_number_len = rt_strlen((char*)number_str);
        RT_ASSERT(phone_number_len>=3);//at least 3 digi
        rt_memcpy( number, number_str, phone_number_len );
    }
    else
    {	
		//use default call center number
        RT_ASSERT(sim900->CenterPhoneNumber != RT_NULL);
        phone_number_len = rt_strlen((char*)sim900->CenterPhoneNumber);
        RT_ASSERT(phone_number_len>=3);//at least 8 digi
        rt_memcpy( number, sim900->CenterPhoneNumber, phone_number_len );
    }

    /*number string --> number & check number validation*/
    for(i=0; i<phone_number_len; i++)
    {
        if( (number[i]>='0') && (number[i]<='9') || (number[i] == '+'))
        {   
			//phone number is valid
            rt_kprintf("%c",number[i]);
        }
        else
        {
            //set comm line free
			rt_kprintf("phone#%d: %c is invalid\n", i, number[i]);
            SetCommLineStatus(STANDBY);
            return RT_ERROR;
        }
     }
    rt_sprintf(str, "ATD%s;\r\n", number);

    /*dail number 5 times*/
    for(i=0; i<5; i++)
    {
        rt_sim900_write(str, rt_strlen(str));	//SEND dial CMD
        rt_kprintf("dailing %s; %02d\n", number, i);

        //	wait for response string
        if(WaitATResp(&CMDresp[0], 5*RT_TICK_PER_SECOND) > 0)
        {
            if(rt_strncmp( (char*)(&CMDresp[0]), "OK", 2) == 0)
            {	
				// if "OK", dail success
                rt_kprintf("dail ok\r\n");
                return RT_EOK;
            }
            else if(rt_strncmp( (char*)(&CMDresp[0]), "ERROR", 5) == 0)
            {	
				//if "error", dail failed
                rt_kprintf("dail error\r\n");
				//set comm line free
                SetCommLineStatus(STANDBY);
                return RT_ERROR;
            }
            else if(rt_strncmp( (char*)(&CMDresp[0]), "NO DIALTONE", 11) == 0)
            {	
				//if "error", dail failed
                rt_kprintf("no dialtone\r\n");
				//set comm line free
                SetCommLineStatus(STANDBY);
                return RT_ERROR;
            }
            else
            {
                rt_kprintf("ATD RECV: %s\r\n",&CMDresp[0]);
            }
        }
    }

	//set comm line free
    SetCommLineStatus(STANDBY);
    return RT_ERROR; 
}


// simple function for waiting for in coming call
// 1=success
static rt_err_t sim900_Call_ComeIn(void)
{
    rt_uint32_t e = 0;
    rt_err_t    err = RT_ERROR;

    //block and waiting for in coming call event
    err = rt_event_recv(&sim900->ATResp_event,
                        RING,
                        RT_EVENT_FLAG_OR|RT_EVENT_FLAG_CLEAR,
                        RT_WAITING_FOREVER, &e);
    return err; 
}


//answer in coming call op
//return RT_ERROR / RT_EOK
static rt_err_t sim900_PickUp(void)
{
    rt_uint8_t  cmd[SIM900_AT_RESP_MQ_MSG_SIZE] = "ATA\r\n";//len=5
    rt_err_t    err = RT_ERROR;
    rt_uint8_t  CMDresp[SIM900_AT_RESP_MQ_MSG_SIZE];

	//if already in call
    if(GetCommLineStatus() == IN_CALL)
        return RT_ERROR;
   
	//	wait for response string
	rt_sim900_write(cmd, rt_strlen((char*)cmd));
    WaitATResp(&CMDresp[0], 5*RT_TICK_PER_SECOND);		
    if(rt_strncmp( (char*)(&CMDresp[0]), "OK", 2) == 0)
    {
        SetCommLineStatus(IN_CALL);
        err = RT_EOK;
    }
    else if(rt_strncmp( (char*)(&CMDresp[0]), "ERROR", 5) == 0)
	{
        err = RT_ERROR;
	}

    return err;
}


//hang up call op
//send hangup cmd
//response handled by finish call func
static rt_err_t sim900_HangUp(void)
{
    rt_uint8_t  cmd[SIM900_AT_RESP_MQ_MSG_SIZE] = "ATH\r\n";//len=5
    rt_uint8_t  CMDresp[SIM900_AT_RESP_MQ_MSG_SIZE];

    if(GetCommLineStatus() == IN_CALL)
	{
        //	wait for response string
		rt_sim900_write(cmd, rt_strlen((char*)&cmd[0]));
		WaitATResp(&CMDresp[0], 5*RT_TICK_PER_SECOND);	
		if(rt_strncmp( (char*)(&CMDresp[0]), "OK", 2) == 0)
		{
			return RT_EOK;
		}
		else if(rt_strncmp( (char*)(&CMDresp[0]), "ERROR", 5) == 0)
		{
			return RT_ERROR;
		}
	}
    return RT_ERROR;
}


//finish call handler
//handle all call finish status
//return RT_ERROR / RT_EOK
static rt_err_t sim900_FinishCall(void)
{
    rt_uint8_t  CMDresp[SIM900_AT_RESP_MQ_MSG_SIZE];
    rt_err_t	status = RT_ERROR;

	// not in call mode
    if(GetCommLineStatus() != IN_CALL)
	{
    	return RT_ERROR;
	}	
    
	//	wait for response string
    WaitATResp(&CMDresp[0], 5*RT_TICK_PER_SECOND);	
    if(rt_strncmp( (char*)(&CMDresp[0]), "OK", 2) == 0)
    {
        rt_kprintf("call hang up\r\n");
        status = RT_EOK;
    }
    else if(rt_strncmp( (char*)(&CMDresp[0]), "BUSY", 4) == 0)
    {
        rt_kprintf("call busy\r\n");
        status = RT_EOK;
    }
    else if(rt_strncmp( (char*)(&CMDresp[0]), "NO ANSWER", 9) == 0)
    {
        rt_kprintf("no answer\r\n");
        status = RT_EOK;
    }
    else if(rt_strncmp( (char*)(&CMDresp[0]), "NO CARRIER", 10) == 0)
    {
        rt_kprintf("call finished\r\n");
        status = RT_EOK;
    }
    else if(rt_strncmp( (char*)(&CMDresp[0]), "ERROR", 5) == 0)
    {
        rt_kprintf("call finished error\r\n");
        status = RT_ERROR;
    }

	//set comm line free
    SetCommLineStatus(STANDBY);
    return status;
}


//test and set registration gsm
//return RT_EOK = registed
static rt_err_t sim900_RegGSM(void)
{
    rt_uint8_t  cmd[SIM900_AT_RESP_MQ_MSG_SIZE];//len=10
    rt_uint8_t  CMDresp[SIM900_AT_RESP_MQ_MSG_SIZE];
    rt_err_t	err = RT_ERROR;

    rt_memset(&cmd[0], 0, sizeof(cmd));
    rt_sprintf((char*)&cmd[0], "AT+CREG?\r\n");
    rt_sim900_write(cmd, rt_strlen((char*)&cmd[0]));

    rt_memset(&CMDresp[0], 0, SIM900_AT_RESP_MQ_MSG_SIZE);
    if(WaitATResp(&CMDresp[0],RT_WAITING_FOREVER) > 0)
    {
        if(rt_strncmp( (char*)(&CMDresp[0]), "+CREG:", 5) == 0)
        {
            rt_memset(&CMDresp[0], 0, SIM900_AT_RESP_MQ_MSG_SIZE);
            if(WaitATResp(&CMDresp[0],RT_WAITING_FOREVER) > 0)
            {
                if(rt_strncmp( (char*)(&CMDresp[0]), "OK", 2) == 0)
                    err = RT_EOK;
            }
        }
        else if(rt_strncmp( (char*)(&CMDresp[0]), "+CME ERROR:", 11) == 0)
		{
            err = RT_ERROR;
		}
    }
    return err;
}


//test and set attach gprs cmd
//return RT_EOK = attached
static rt_err_t sim900_AttachGPRS(void)
{
    rt_uint8_t  cmd[SIM900_AT_RESP_MQ_MSG_SIZE];//len=10
    rt_uint8_t  CMDresp[SIM900_AT_RESP_MQ_MSG_SIZE];
    rt_err_t    err = RT_ERROR;

    rt_memset(&cmd[0], 0, sizeof(cmd));
    rt_sprintf((char*)&cmd[0], "AT+CGATT?\r\n");
    rt_sim900_write(cmd, rt_strlen((char*)&cmd[0]));

    rt_memset(&CMDresp[0], 0, SIM900_AT_RESP_MQ_MSG_SIZE);
    if(WaitATResp(&CMDresp[0],RT_WAITING_FOREVER) > 0)
    {
        if(rt_strncmp( (char*)(&CMDresp[0]), "+CGATT: 1", 9) == 0)
		{
            err = RT_EOK;
		}
        else if(rt_strncmp( (char*)(&CMDresp[0]), "+CGATT: 0", 9) == 0)
		{
        	err = RT_ERROR;
		}

        rt_memset(&CMDresp[0], 0, SIM900_AT_RESP_MQ_MSG_SIZE);
        if(WaitATResp(&CMDresp[0], 8) > 0)
        {
            if(rt_strncmp( (char*)(&CMDresp[0]), "OK", 2) == 0)
			{
                rt_kprintf("CGATT? DONE\r\n");
			}
        }
    }

    while(err == RT_ERROR)
    {
        rt_memset(&cmd[0], 0, sizeof(cmd));
        rt_sprintf((char*)&cmd[0], "AT+CGATT=1\r\n");
        rt_sim900_write(cmd, rt_strlen((char*)&cmd[0]));
        rt_memset(&CMDresp[0], 0, SIM900_AT_RESP_MQ_MSG_SIZE);
        if(WaitATResp(&CMDresp[0],RT_WAITING_FOREVER) > 0)
        {
            if(rt_strncmp( (char*)(&CMDresp[0]), "OK", 2) == 0)
			{

			}
            else if(rt_strncmp( (char*)(&CMDresp[0]), "+CME", 4) == 0)
			{

			}
            else if(rt_strncmp( (char*)(&CMDresp[0]), "ERROR", 5) == 0)
			{
				rt_thread_delay(5*RT_TICK_PER_SECOND);
			}
        }

        rt_memset(&cmd[0], 0, sizeof(cmd));
        rt_sprintf((char*)&cmd[0], "AT+CGATT?\r\n");
        rt_sim900_write(cmd, rt_strlen((char*)&cmd[0]));
        rt_memset(&CMDresp[0], 0, SIM900_AT_RESP_MQ_MSG_SIZE);
        if(WaitATResp(&CMDresp[0],RT_WAITING_FOREVER) > 0)
        {
            if(rt_strncmp( (char*)(&CMDresp[0]), "+CGATT: 1", 9) == 0)
			{
                err = RT_EOK;
			}
            else if(rt_strncmp( (char*)(&CMDresp[0]), "+CGATT: 0", 9) == 0)
			{
                err = RT_ERROR;
			}

            rt_memset(&CMDresp[0], 0, SIM900_AT_RESP_MQ_MSG_SIZE);
            if(WaitATResp(&CMDresp[0],RT_WAITING_FOREVER) > 0)
            {
                if(rt_strncmp( (char*)(&CMDresp[0]), "OK", 2) == 0)
				{
                    rt_kprintf("CGATT? DONE\r\n");
				}
            }
        }
    }

    return err;
}


/********************************************
* This function START TASK                  *
* set apn,username,password                 *
* @return the byte number of sim900 sent.   *
********************************************/
static rt_err_t sim900_CSTT(rt_uint8_t* APN, rt_uint8_t* USER, rt_uint8_t* PASSWORD)
{
    rt_uint8_t  cmd[SIM900_AT_RESP_MQ_MSG_SIZE];//len=10
    rt_uint8_t  CMDresp[SIM900_AT_RESP_MQ_MSG_SIZE];
    rt_err_t	state = RT_ERROR;
    
    rt_memset(&cmd[0], 0, sizeof(cmd));
    rt_sprintf( (char*)&cmd[0],
                "%s\"%s\",\"%s\",\"%s\"\r\n",
                "AT+CSTT=", APN, USER, PASSWORD );
    rt_sim900_write(cmd, rt_strlen((char*)&cmd[0]));
    rt_memset(&CMDresp[0], 0, SIM900_AT_RESP_MQ_MSG_SIZE);
    
    if(WaitATResp(&CMDresp[0],RT_WAITING_FOREVER) > 0)
    {
        if(rt_strncmp( (char*)(&CMDresp[0]), "OK", 2) == 0)
		{
            state = RT_EOK;
		}
        else if(rt_strncmp( (char*)(&CMDresp[0]), "ERROR", 5) == 0)
		{
            state = RT_ERROR;
		}
    }
    return state;
}


/********************************************
* This function bring up gprs               *
*                                           *
* @return RT_EOK = success.                 *
********************************************/
static rt_err_t sim900_CIICR(void)
{
    rt_uint8_t  cmd[SIM900_AT_RESP_MQ_MSG_SIZE] = "AT+CIICR\r\n";//len=10
    rt_uint8_t  CMDresp[SIM900_AT_RESP_MQ_MSG_SIZE];
    rt_err_t	state = RT_ERROR;

    rt_sim900_write(cmd, rt_strlen((char*)&cmd[0]));
    rt_memset(&CMDresp[0], 0, SIM900_AT_RESP_MQ_MSG_SIZE);
    if(WaitATResp(&CMDresp[0],RT_WAITING_FOREVER) > 0)
    {
        if(rt_strncmp( (char*)(&CMDresp[0]), "OK", 2) == 0)
		{
            state = RT_EOK;
		}
        else if(rt_strncmp( (char*)(&CMDresp[0]), "ERROR", 5) == 0)
		{
            state = RT_ERROR;
		}
    }
    return state;
}


/********************************************
* This function get local ip addr           *
*                                           *
* @return RT_EOK = success.                 *
********************************************/
static rt_err_t sim900_CIFSR(void)
{
    rt_uint8_t  cmd[SIM900_AT_RESP_MQ_MSG_SIZE] = "AT+CIFSR\r\n";//len=10
    rt_uint8_t  CMDresp[SIM900_AT_RESP_MQ_MSG_SIZE];
    rt_err_t	state = RT_ERROR;

    rt_sim900_write(cmd, rt_strlen((char*)&cmd[0]));
    rt_memset(&CMDresp[0], 0, SIM900_AT_RESP_MQ_MSG_SIZE);
    if(WaitATResp(&CMDresp[0],RT_WAITING_FOREVER) > 0)
    {
        if(rt_strncmp( (char*)(&CMDresp[0]), "ERROR", 5) == 0)
		{
            state = RT_ERROR;
		}
        else if(CMDresp[0] >= '0' || CMDresp[0] <= '9')
        {
            state = RT_EOK;
            sim900_set_attribute(&(sim900->local_addr), &CMDresp[0]);
        }
    }
    return state;
}


/********************************************
* This function query current conn status   *
*                                           *
* @return the byte number of sim900 sent.   *
********************************************/
rt_uint8_t sim900_CIPStatus(void)
{
    rt_uint8_t  cmd[SIM900_AT_RESP_MQ_MSG_SIZE] = "AT+CIPSTATUS\r\n";
    rt_uint8_t  CMDresp[SIM900_AT_RESP_MQ_MSG_SIZE];
    rt_err_t    err = RT_ERROR;

    rt_sim900_write(cmd, rt_strlen((char*)&cmd[0]));
    rt_memset(&CMDresp[0], 0, SIM900_AT_RESP_MQ_MSG_SIZE);
    if(WaitATResp(&CMDresp[0], 10*RT_TICK_PER_SECOND) > 0)
    {
        if(rt_strncmp( (char*)(&CMDresp[0]), "OK", 2) == 0)
        {
            rt_memset(&CMDresp[0], 0, SIM900_AT_RESP_MQ_MSG_SIZE);
            if(WaitATResp(&CMDresp[0], 10*RT_TICK_PER_SECOND) > 0)
            {
                if(rt_strncmp( (char*)(&CMDresp[0]), "STATE:", 6) == 0)
				{
                    err = RT_EOK;
				}
            }
        }
        else if(rt_strncmp( (char*)(&CMDresp[0]), "ERROR", 5) == 0)
		{
            err = RT_ERROR;
		}
    }
    return err;
}


/********************************************
* This function establish tcp/udp conn      *
*                                           *
* @return the byte number of sim900 sent.   *
********************************************/
static rt_err_t sim900_ConnStart(rt_uint8_t* TYPE, rt_uint8_t* IP, rt_uint8_t* PORT)
{
    rt_uint8_t  cmd[50];
    rt_uint8_t  CMDresp[SIM900_AT_RESP_MQ_MSG_SIZE];
    rt_err_t    state = RT_ERROR;
    rt_uint8_t  i;

    rt_sprintf((char*)cmd, "%s\"%s\",\"%s\",\"%s\"\r\n", "AT+CIPSTART=", TYPE, IP, PORT);
    rt_kprintf("sim900_ConnStart:%s", cmd);

    for (i = 0; i < 10; i++)
    {	
		//try 10 times
    	rt_kprintf("sim900 Connecting: %d\r\n", i);
        rt_sim900_write(cmd, rt_strlen((char*)&cmd[0]));
        rt_memset(&CMDresp[0], 0, SIM900_AT_RESP_MQ_MSG_SIZE);
        if(WaitATResp(&CMDresp[0], 10*RT_TICK_PER_SECOND) > 0)
        {
            if(rt_strncmp( (char*)(&CMDresp[0]), "OK", 2) == 0)
            {	
				//wait for "OK"
                rt_memset(&CMDresp[0],0,SIM900_AT_RESP_MQ_MSG_SIZE);
                if(WaitATResp(&CMDresp[0], 15*RT_TICK_PER_SECOND) > 0)
                {	
					//wait for if "connect ok" then conn ok
                    if(rt_strncmp( (char*)(&CMDresp[0]), "CONNECT OK", 10) == 0)
                    {
                        rt_kprintf("sim900_ConnStart: CONNECT OK\r\n");
                        return RT_EOK;
                    }
                    else if(rt_strncmp( (char*)(&CMDresp[0]), "ALREADY CONNECT", 15) == 0)
                    {
                        rt_kprintf("sim900_ConnStart: ALREADY CONNECT\r\n");
                        return RT_ERROR;
                    }
                    else if(rt_strncmp( (char*)(&CMDresp[0]), "STATE:", 6) == 0)
                    {   
						//if "state: tcp closed" and then "connect fail" then conn fail
                        rt_memset(&CMDresp[0],0,SIM900_AT_RESP_MQ_MSG_SIZE);
                        if(WaitATResp(&CMDresp[0], 5*RT_TICK_PER_SECOND) > 0)
                        {
                            if(rt_strncmp( (char*)(&CMDresp[0]), "CONNECT FAIL", 12) == 0)
							{
                                rt_kprintf("sim900_ConnStart: CONNECT FAIL\r\n");
							}
                            return RT_ERROR;
                        }
                    }
                }
                state = RT_EOK;
            }
            else if(rt_strncmp( (char*)(&CMDresp[0]), "ERROR", 5) == 0)
			{
                state = RT_ERROR;
			}
            else if(rt_strncmp( (char*)(&CMDresp[0]), "+CME", 4) == 0)
			{
                state = RT_ERROR;
			}
        }
    }

    return state;   
}


/********************************************
* This function close tcpudp                *
* use before cip shut                       *
* @return the byte number of sim900 sent.   *
********************************************/
static rt_err_t sim900_CIPCLOSE(void)
{
    rt_uint8_t  cmd[SIM900_AT_RESP_MQ_MSG_SIZE] = "AT+CIPCLOSE\r\n";//len=12
    rt_uint8_t  CMDresp[SIM900_AT_RESP_MQ_MSG_SIZE];
    rt_err_t    err = RT_ERROR;

    rt_sim900_write(cmd, rt_strlen((char*)&cmd[0]));
    rt_memset(&CMDresp[0], 0, SIM900_AT_RESP_MQ_MSG_SIZE);
    if(WaitATResp(&CMDresp[0], 5*RT_TICK_PER_SECOND) > 0)
    {
        if( rt_strncmp( (char*)(&CMDresp[0]), "CLOSE OK", 8) == 0 )
		{
            err = RT_EOK;
		}
        else if( rt_strncmp( (char*)(&CMDresp[0]), "ERROR", 5) == 0 )
		{
            err = RT_ERROR;
		}
    }
    return err;
}


/********************************************
* This function deactive gprs               *
* use before tcp/ucp connection             *
* @return the byte number of sim900 sent.   *
********************************************/
static rt_uint8_t sim900_CIPSHUT(void)
{
    rt_uint8_t  cmd[SIM900_AT_RESP_MQ_MSG_SIZE] = "AT+CIPSHUT\r\n";//len=12
    rt_uint8_t  CMDresp[SIM900_AT_RESP_MQ_MSG_SIZE];
    rt_err_t    err = RT_ERROR;

    rt_sim900_write(cmd, rt_strlen((char*)&cmd[0]));
    rt_memset(&CMDresp[0], 0, SIM900_AT_RESP_MQ_MSG_SIZE);
    if(WaitATResp(&CMDresp[0], 5*RT_TICK_PER_SECOND) > 0)
    {
        if(rt_strncmp( (char*)(&CMDresp[0]), "SHUT OK", 7) == 0)
		{
            err = RT_EOK;
		}
        else if(rt_strncmp( (char*)(&CMDresp[0]), "ERROR", 5) == 0)
		{
            err = RT_ERROR;
		}
    }
    return err;
}


/********************************************
* This function signal qualicy check        *
* call by sim900_control funcs              *
* @return 1 success, 0 failed.     			*
********************************************/
//simple function to transfer rssi 2 signal DB number
//return: -115~-52
static rt_int8_t rssi2db(rt_uint8_t rssi)
{
    rt_int8_t db = 0;
    if(rssi == 0)
        db = -115;
    else if (rssi == 1)
        db = -111;
    else if (rssi >=2 && rssi <=30)
        db = 2*rssi-114;
    else if (rssi >= 31 && rssi < 99)
        db = -52;
    else
        db = 0;
    return db;
}

static rt_err_t sim900_SigCheck(void)
{
    rt_uint8_t  SQ[5]; //signal quality
    rt_uint8_t  state = RT_ERROR;
    rt_uint8_t  i = 0;
    rt_uint8_t  cmd[SIM900_AT_RESP_MQ_MSG_SIZE];
    rt_uint8_t  CMDresp[SIM900_AT_RESP_MQ_MSG_SIZE];

    /*send sq cmd*/
    rt_memset(&cmd[0], 0, sizeof(cmd));
    rt_sprintf((char*)&cmd[0], "AT+CSQ\r\n");

    for(i = 0; (i<5)&&(state!=RT_EOK); i++)
    {
        rt_sim900_write(&cmd[0], rt_strlen((char*)&cmd[0]));
        rt_memset(&CMDresp[0], 0, SIM900_AT_RESP_MQ_MSG_SIZE);
        if(WaitATResp(&CMDresp[0], 5*RT_TICK_PER_SECOND) > 0)
        {
            if(rt_strncmp( (char*)(&CMDresp[0]), "+CSQ:", 5) == 0)
            {
                sscanf( (char*)(&CMDresp[0]), "%*[^ ] %[^,]", SQ);//etc:"+CSQ: 24,0"
                rt_kprintf("signal:%s\r\n", SQ);
                sim900->rssi = atoi((const char *)SQ);  //SQ:0--99
                sim900->signalDB = rssi2db(sim900->rssi);
                rt_memset(&CMDresp[0], 0, SIM900_AT_RESP_MQ_MSG_SIZE);
                if(WaitATResp(&CMDresp[0], 5*RT_TICK_PER_SECOND) > 0)
                {
                    if(rt_strncmp( (char*)(&CMDresp[0]), "OK", 2) == 0)
					{
                        rt_kprintf("signal read OK\r\n");
					}
                    state = RT_EOK;
                }
            }
            else if(rt_strncmp( (char*)(&CMDresp[0]), "+CME", 4) == 0)//"+CME ERROR:"
            {
                rt_kprintf("signal: %s\r\n", &CMDresp[0]);
            }
            else if(rt_strncmp( (char*)(&CMDresp[0]), "ERROR", 5) == 0)//"ERROR"
            {
                rt_kprintf("signal read CMD error\r\n");
            }
        }
    }
    return state;
}


/********************************************
* This function ask for sim900 ID info      *
* use when sim900 startup                   *
* @return RT_EOK / RT_ERROR.                *
********************************************/
//simple function for set sim900 device attributes
static void sim900_set_attribute(rt_uint8_t** attribute, rt_uint8_t* content)
{
    rt_uint8_t  len = 0;
    /*set default connection type: tcp/udp*/
    if(*attribute != RT_NULL)
        rt_free_align( (char*)attribute );
    len = rt_strlen((char*)content) + 1;
    *attribute = rt_malloc_align(len, RT_ALIGN_SIZE);
	rt_memset((char*)*attribute, 0, len);
    rt_strncpy( (char*)*attribute, (char*)content, len );
}


/*RT-Thread Device Driver Interface*/
/****************************************
*   rt_sim900_init                      *
*   AT CMD setup sim900 moduler         *
*   1. echo off                         *
*   2. ip head on                       *
*   3. read info                        *
****************************************/
rt_err_t rt_sim900_init(void)
{
    rt_uint8_t  cmd[SIM900_AT_RESP_MQ_MSG_SIZE];
    rt_uint8_t  CMDresp[SIM900_AT_RESP_MQ_MSG_SIZE];
    rt_err_t	state = RT_ERROR;

    /*set default connection type: tcp/udp*/
    sim900_set_attribute(&(sim900->conn_type), &default_connTYPE1[0]);

    /*set default remote ip addr*/
    sim900_set_attribute(&(sim900->remote_addr), &default_remoteIPADDR1[0]);

    /*set default remote port*/
    sim900_set_attribute(&(sim900->remote_port), &default_remotePORT1[0]);

    /*set default outgoing phone number*/
    sim900_set_attribute(&(sim900->CenterPhoneNumber), &default_PhoneNumber[0]);

    /*set auto connection off*/
    sim900->tcpudp_autoconn = 0;
    sim900->signalDB = 0;
    sim900->rssi = 0;

    rt_memset(&cmd[0], 0, sizeof(cmd));
    rt_memset(&CMDresp[0], 0, SIM900_AT_RESP_MQ_MSG_SIZE);

    /*set eoch off*/
    rt_sprintf((char*)&cmd[0], "ATE0\r\n");
    do
    {
        rt_sim900_write(&cmd[0], rt_strlen((char*)&cmd[0]));
        if(WaitATResp(&CMDresp[0], 5*RT_TICK_PER_SECOND) > 0)
        {	
			// get response
            if(rt_strncmp( (char*)(&CMDresp[0]), "OK", 2) == 0)
            {
                rt_kprintf("echo set off\r\n");
                state = RT_EOK;
            }
            else if (rt_strncmp( (char*)(&CMDresp[0]), "ERROR", 5) == 0)
            {
                rt_kprintf("echo set off error, check comm line\r\n");
                return RT_ERROR;
            }
        }
    }while(state != RT_EOK);

    state = RT_ERROR;
    rt_memset(&cmd[0], 0, sizeof(cmd));
    rt_memset(&CMDresp[0], 0, SIM900_AT_RESP_MQ_MSG_SIZE);

    /*IP DATA HEAD set on*/
    rt_sprintf((char*)&cmd[0], "AT+CIPHEAD=1\r\n");
    do
    {
        rt_sim900_write(&cmd[0], rt_strlen((char*)&cmd[0]));
        if(WaitATResp(&CMDresp[0], 5*RT_TICK_PER_SECOND) > 0)
        {	
			// get response
            if(rt_strncmp( (char*)(&CMDresp[0]), "OK", 2) == 0)
            {
                rt_kprintf("ip head set on\r\n");
                state = RT_EOK;     //success
            }
            else if(rt_strncmp( (char*)(&CMDresp[0]), "ERROR", 5) == 0)
            {
                rt_kprintf("ip head set error\r\n");
                return RT_ERROR;    //failed
            }
        }
    }while(state != RT_EOK);

    return state;
}

rt_err_t rt_sim900_open(void)
{
    light_init();
	gsm_driver_init();

	gsm_reset();
	return RT_EOK;
}

rt_err_t rt_sim900_close(void)
{
    return RT_EOK;
}

rt_size_t rt_sim900_read(void* buffer)
{
	return RT_EOK;
}


/********************************************
* This function write ATcmd to sim900       *
* @return the byte number of sim900 sent.   *
********************************************/
rt_size_t rt_sim900_write(const void* buffer, rt_size_t size)
{
    rt_size_t write_len;
    SIM900_LOCK();
	RT_DEBUG_LOG(DEBUG_SIM900, ("-----> %s", buffer));
    write_len = rt_device_write(sim900->device, 0, buffer, size);
    SIM900_UNLOCK();
    return write_len;
}


rt_err_t rt_sim900_control(rt_uint8_t cmd, void *args)
{
    rt_err_t    err = RT_ERROR;
    rt_uint8_t  sim900_cmd = cmd;
    rt_uint32_t e = 0;

    rt_uint8_t  CMD[16];//cmd string < 16 char

    switch(sim900_cmd)
    {
        case 0:    //bring up gsm gprs and ready for tcpudp conn
            err = sim900_RegGSM();
            err = sim900_AttachGPRS();
			err = sim900_CSTT("3GNET","","");
            err = sim900_CIICR();
            err = sim900_CIFSR();
            break;

        case 1:    //DO_TCPUDP_CONN
            err = sim900_ConnStart(sim900->conn_type, sim900->remote_addr, sim900->remote_port);
            break;

        case 2:    //TCPUDP_DISCONN:
            err = sim900_CIPCLOSE();
            break;

        case 3:    //TCPUDP_CLOSED:
            err = sim900_CIPSHUT();
            break;

        case 4:    //WAITFOR_CONN_CLOSED wait for server to close the connection
            err = rt_event_recv(&sim900->ATResp_event,
                                CLOSED,
                                RT_EVENT_FLAG_OR|RT_EVENT_FLAG_CLEAR,
                                RT_WAITING_FOREVER, &e);
            break;

        case 5:    //TCPUDP_SET_CONN_TYPE:  TCP/UDP
            if(args != RT_NULL)
            {
                sim900_set_attribute(&(sim900->conn_type), (rt_uint8_t*)args);
                err = RT_EOK;
            }
            else
			{
                err = RT_ERROR;
			}
            break;

        case 6:    //TCPUDP_AUTO_CONN
            if(args != RT_NULL)
            {
                rt_memset(&CMD[0], 0, sizeof(CMD));
                rt_strncpy((char*)&CMD[0], (char*)args, rt_strlen((char*)args));
                sim900->tcpudp_autoconn = CMD[0];
                err = RT_EOK;
            }
            else
			{
                err = RT_ERROR;
			}
            break;

        case 7:    //set TCPUDP_REMOTE_IP
            if(args != RT_NULL)
            {
                sim900_set_attribute(&(sim900->remote_addr), (rt_uint8_t*)args);
                err = RT_EOK;
            }
            else
			{
                err = RT_ERROR;
			}
            break;

        case 8:    //set TCPUDP_REMOTE_PORT
            if(args != RT_NULL)
            {
                sim900_set_attribute(&(sim900->remote_port), (rt_uint8_t*)args);
                err = RT_EOK;
            }
            else
			{
                err = RT_ERROR;
			}
            break;

        case 9:    //QUERY_AUTO_CONN
            if(args != RT_NULL)
            {
                rt_strncpy((char*)args, (char*)&sim900->tcpudp_autoconn, 1);
            }
            break;

        case 11:           
            if(args != RT_NULL)
			{
                rt_memset(&CMD[0], 0, sizeof(CMD));
				rt_strncpy((char*)&CMD[0], (char*)args, rt_strlen((char*)args));
			}
            err = sim900_Call(&CMD[0]);//send dial call cmd
            //return call done status
            break;

        case 12:    //PICKUP_CALL
            err = sim900_PickUp();//send answer call cmd
            //return call done status
            break;

        case 13:    //HANGUP_CALL
            err = sim900_HangUp();//send hang up call cmd
            break;

        case 14:    //FINISH_CALL
            err = sim900_FinishCall();// do finish call ops
            break;

        case 15:    //WAITFOR_IN_CALL
            err = sim900_Call_ComeIn();  //wait for incoming call
            break;

        case 31:    //DO_SIG_CHECK
            err = sim900_SigCheck();
            if(err == RT_EOK && args != RT_NULL)
			{
                rt_strncpy(args, (char *)&sim900->rssi, 1);	 
			}
            break;

        default:
        break;
    }

    return err;
}


/****************************************
*   rt_sim900_rx_ind                    *
*   hook func in usart isr              *
****************************************/
static rt_err_t rt_sim900_rx_ind(rt_device_t dev, rt_size_t size)
{
    RT_ASSERT(sim900 != RT_NULL);
    /* release semaphore to notice thread parser rx data */
    rt_sem_release(&sim900->rx_semaphore);
    return RT_EOK;
}


/************************************************
* This func set the I/O device for sim900       *
*   set usart device as virtual sim900          *
************************************************/
static void rt_sim900_set_device(const char* device_name)
{
    rt_device_t dev = RT_NULL;
    RT_ASSERT(sim900 != RT_NULL);

    /* find usart device */
    dev = rt_device_find(device_name);
    if (dev != RT_NULL && rt_device_open(dev, RT_DEVICE_OFLAG_RDWR) == RT_EOK)
    {
        if (sim900->device != RT_NULL)
        {   
			/* close old sim900 virtual device */
            rt_device_close(sim900->device);
        }
        /* set new sim900 virtual device */
        sim900->device = dev;
        rt_device_set_rx_indicate(dev, rt_sim900_rx_ind);
    }
    else
    {
        rt_kprintf("sim900: can not find device:%s\n", device_name);
    }
}


/********************************************************
* This function returns current sim900 ported device.   *
* @return the sim900 ported device name is returned.    *
********************************************************/
const char* rt_sim900_get_device(void)
{
    RT_ASSERT(sim900 != RT_NULL);
    return sim900->device->parent.name;
}


//simple function for IP data buffer init
static void IPDataBuff_init(void)
{
    IPdata_buff = (rt_uint8_t*)rt_malloc_align(1460, RT_ALIGN_SIZE);
    RT_ASSERT(IPdata_buff != RT_NULL);
    RT_ASSERT(&IPdata_rb != RT_NULL);
    rb_init(&IPdata_rb, &IPdata_buff[0], sizeof(IPdata_buff));
}


/****************************************
*   rt_hw_sim900_init                   *
*   init virtual device sim900          *
*   1. allocate mem                     *
*   2. init semaphore and msg queue     *
****************************************/
void rt_hw_sim900_init(const char* device_name)
{
    /*init sim device mem*/
    if(sim900 == RT_NULL)
	{
        sim900 = (rt_sim900_device*)rt_malloc_align(sizeof(rt_sim900_device), RT_ALIGN_SIZE);
	}
    rt_memset(sim900, 0, sizeof(rt_sim900_device));

    SetCommLineStatus(INITIAL);

    /*init ip data buffer mem*/
    IPDataBuff_init();

    /*init sem*/
    RT_ASSERT( rt_sem_init(&sim900->rx_semaphore, "s9_RXsem", 0, RT_IPC_FLAG_FIFO) == RT_EOK );
    RT_ASSERT( rt_sem_init(&sim900_lock, "s9_lock", 1, RT_IPC_FLAG_FIFO) == RT_EOK );
    RT_ASSERT( rt_sem_init(&rb_lock, "rb_lock", 1, RT_IPC_FLAG_FIFO) == RT_EOK );
    RT_ASSERT( rt_sem_init(&sim900->frame_sem, "s9_Fsem", 0, RT_IPC_FLAG_FIFO) == RT_EOK );
    RT_ASSERT( rt_event_init(&sim900->ATResp_event, "C_RSPevt", RT_IPC_FLAG_FIFO) == RT_EOK );

    //at response msg queue.
    sim900->AT_resp_MQ_poll = (rt_uint8_t*)rt_malloc_align(SIM900_AT_RESP_MQ_POOL_SIZE, RT_ALIGN_SIZE);
    RT_ASSERT(sim900->AT_resp_MQ_poll != RT_NULL);
    RT_ASSERT( rt_mq_init(&sim900->AT_resp_MQ, "s9_ATRMQ", sim900->AT_resp_MQ_poll, SIM900_AT_RESP_MQ_MSG_SIZE-sizeof(void*), SIM900_AT_RESP_MQ_POOL_SIZE, RT_IPC_FLAG_FIFO) == RT_EOK );

    //set device
    rt_sim900_set_device(device_name);
}

#endif
