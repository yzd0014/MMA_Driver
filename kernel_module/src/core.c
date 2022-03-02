//////////////////////////////////////////////////////////////////////
//                     University of California, Riverside
//
//
//
//                             Copyright 2022
//
////////////////////////////////////////////////////////////////////////
//
// This program is free software; you can redistribute it and/or modify it
// under the terms and conditions of the GNU General Public License,
// version 2, as published by the Free Software Foundation.
//
// This program is distributed in the hope it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
// more details.
//
// You should have received a copy of the GNU General Public License along with
// this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
//
////////////////////////////////////////////////////////////////////////
//
//   Author:  Hung-Wei Tseng, Yu-Chia Liu
//
//   Description:
//     Core of Kernel Module for CSE202's Resource Container
//
////////////////////////////////////////////////////////////////////////

#include "blockmma.h"
#include <asm/segment.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/poll.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/mutex.h>
#include "core.h"

extern struct miscdevice blockmma_dev;
static struct Node
{
    float *k_a;
    float *k_b;
    float *k_c;
    float *u_c;
    int m;
    int n;
    int k;
    int mma_tid;//this is used to make sure resutls from mma is send to correct bucket in kernel
    int user_tid;
    int finished; 
    struct Node *next;
    struct Node *prev;
} head;
static struct Node *tail;
static struct Node *queueHead;
static int tile;
struct mutex list_lock;
/**
 * Enqueue a task for the caller/accelerator to perform computation.
 */
long blockmma_send_task(struct blockmma_cmd __user *user_cmd)
{
    struct blockmma_cmd cmd;
    copy_from_user(&cmd, user_cmd, sizeof(cmd));
    
    tile = (int)user_cmd->tile;
    long output = (int)current->pid;;
    
    //find if c is already in linked list or not
    mutex_lock(&list_lock);
    struct Node *p;
    p = head.next;
    int createNewNode = 1;
    while(p)
    {
        if(p->u_c == user_cmd->c)
        {
            createNewNode = 0;
            if(p->finished == 0)
            {
                output = -1;
            }
            else
            {
                int N = p->n;
                int K = p->k;
                //update node
                int i;
                for(i = 0; i < tile; i++)
                {
                    float *u_a = (float *)user_cmd->a;
                    float *u_b = (float *)user_cmd->b;
                    copy_from_user(p->k_a+i*tile, u_a+i*N, tile*sizeof(float));
                    copy_from_user(p->k_b+i*tile, u_b+i*K, tile*sizeof(float));
                }                    
                p->finished = 0;
                
                //put node at the end of queue
                p->prev->next = p->next;
                p->next->prev = p->prev;
                tail->next = p;
                p->prev = tail;
                p->next = NULL;
                tail = p;
            }
            break;
        }
        p = p->next;
    }
    
    if(createNewNode)
    {
        //printk("1: create new node!\n");
        struct Node *p;
        p = kmalloc(sizeof(*p), GFP_ATOMIC);
        int M = (int)user_cmd->m;
        int N = (int)user_cmd->n;
        int K = (int)user_cmd->k;
        float *k_a = kmalloc(sizeof(float)*tile*tile, GFP_ATOMIC);
        float *k_b = kmalloc(sizeof(float)*tile*tile, GFP_ATOMIC);
        float *k_c = kmalloc(sizeof(float)*tile*tile, GFP_ATOMIC);
        int i;
        for(i = 0; i < tile; i++)
        { 
            float *u_a = (float *)user_cmd->a;
            float *u_b = (float *)user_cmd->b;
            float *u_c = (float *)user_cmd->c;
            copy_from_user(k_a+i*tile, u_a+i*N, tile*sizeof(float));
            copy_from_user(k_b+i*tile, u_b+i*K, tile*sizeof(float));
            copy_from_user(k_c+i*tile, u_c+i*K, tile*sizeof(float));
        }
        p->k_a = k_a;
        p->k_b = k_b;
        p->k_c = k_c;
        p->u_c = (float *)user_cmd->c;
        p->m = M;
        p->n = N;
        p->k = K;
        p->mma_tid = -1;//TODO
        p->user_tid = (int)current->pid;
        p->finished = 0;
        p->next = NULL;
        p->prev = tail;
        tail->next = p;
        tail = p;
        if(queueHead == NULL)
        {
            queueHead = p;
        }
    }
    mutex_unlock(&list_lock);
    
    //printk("1: exit send task lock!\n");
    return output;
}

/**
 * Return until all outstanding tasks for the calling process are done
 */
int blockmma_sync(struct blockmma_cmd __user *user_cmd)
{
    int foundOutstanding = 0;
    int output = 0;
    int m_tid = (int)current->pid;
    
    mutex_lock(&list_lock);
    struct Node *p = head.next;
    while(p != NULL)
    {
        if(p->user_tid == m_tid && p->finished)
        {
            int i;
            //printk("4: kernel finds what to return!\n");
            int K = p->k;
            for(i = 0; i < tile; i++)
            {
                float *u_c = (float *)p->u_c;
                copy_to_user(u_c+i*K, p->k_c+i*tile, tile*sizeof(float));
            }
            
            //delete node
            if(p == tail)
            {
               tail = p->prev; 
            } 
            p->prev->next = p->next;
            if(p->next != NULL) p->next->prev = p->prev;
            struct Node* temp = p->next;
            kfree(p->k_a);
            kfree(p->k_b);
            kfree(p->k_c);
            kfree(p);
            p = temp;
        }
        else if (p->user_tid == m_tid && !p->finished)
        {
            foundOutstanding = 1;
            break;
        }
        else
        {
            p = p->next;
        }
    }
    mutex_unlock(&list_lock);
    
    if(!foundOutstanding)
    {
        output = (int)current->pid;
        //printk("4: all results are collected!\n");
    }
    else
    {
        output = -1;
    }
    
    return output;
}

/**
 * Return the task id of a task to the caller/accelerator to perform computation.
 */
int blockmma_get_task(struct blockmma_hardware_cmd __user *user_cmd)
{
    int output;
    
    mutex_lock(&list_lock);
    if(queueHead)
    {
        output = (int)current->pid;
        queueHead->mma_tid = output;//TODO
        int i;
        for(i = 0; i < tile; i++)
        { 
            float *u_a = (float *)user_cmd->a;
            float *u_b = (float *)user_cmd->b;
            float *u_c = (float *)user_cmd->c;
            copy_to_user(u_a+i*tile, queueHead->k_a+i*tile, tile*sizeof(float));
            copy_to_user(u_b+i*tile, queueHead->k_b+i*tile, tile*sizeof(float));
            copy_to_user(u_c+i*tile, queueHead->k_c+i*tile, tile*sizeof(float));
        }

        //update queue
        queueHead = queueHead->next;
        //printk("2: mma gets a task!\n");
    }
    else
    {
        output = -1;
    }
    mutex_unlock(&list_lock);
    
    return output;
}


/**
 * Return until the task specified in the command is done.
 */
int blockmma_comp(struct blockmma_hardware_cmd __user *user_cmd)
{
    struct Node *p;
    int found = 0;
    int output = 0;
    
    mutex_lock(&list_lock);
    p = head.next;
    while(p != NULL)
    {
        if(p->mma_tid == user_cmd->tid)//TODO
        {
            found = 1;
            int i;
            for(i = 0; i < tile; i++)
            {
                float *u_c = (float *)user_cmd->c;
                copy_from_user(p->k_c+i*tile, u_c+i*tile, tile*sizeof(float));
            }
            p->finished = 1;
            break;
        }   
        p = p->next;
    }
    mutex_unlock(&list_lock);
    
    if(found)
    {
        output = (int)current->pid;
        //printk("3: kernel get results from mma!\n");
    }
    else
    {
        output = -1;
        printk("blockmma_comp fails!\n");
    }
    return output;
}

/*
 * Tell us who wrote the modulead
 */
int blockmma_author(struct blockmma_hardware_cmd __user *user_cmd)
{
    struct blockmma_hardware_cmd cmd;
    char authors[] = "R9";
    if (copy_from_user(&cmd, user_cmd, sizeof(cmd)))
    {
        return -1;
    }
    copy_to_user((void *)cmd.a, (void *)authors, sizeof(authors));
    return 0;
}

int blockmma_init(void)
{
    int ret =0;
    if ((ret = misc_register(&blockmma_dev)))
    {
        printk(KERN_ERR "Unable to register \"blockmma\" misc device\n");
        return ret;
    }
    mutex_init(&list_lock);
    head.next = NULL;
    head.prev = NULL;
    tail = &head;
    queueHead = head.next;
    printk("BlockMMA kernel module installed\n");
    return ret;
}

void blockmma_exit(void)
{
    if(head.next != NULL)
    {
        printk("Not all memory is released!\n");
    }
    printk("BlockMMA removed\n");
    misc_deregister(&blockmma_dev);
}

