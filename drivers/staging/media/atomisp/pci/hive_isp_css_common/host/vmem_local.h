/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Support for Intel Camera Imaging ISP subsystem.
 * Copyright (c) 2010-2015, Intel Corporation.
 */

#ifndef __VMEM_LOCAL_H_INCLUDED__
#define __VMEM_LOCAL_H_INCLUDED__

#include "type_support.h"
#include "vmem_global.h"

typedef u16 t_vmem_elem;
typedef s16 t_svmem_elem;

#define VMEM_ARRAY(x, s)	t_vmem_elem x[(s) / ISP_NWAY][ISP_NWAY]
#define SVMEM_ARRAY(x, s)	t_svmem_elem x[(s) / ISP_NWAY][ISP_NWAY]

void isp_vmem_load(
    const isp_ID_t		ID,
    const t_vmem_elem	*from,
    t_vmem_elem		*to,
    unsigned int elems); /* In t_vmem_elem */

void isp_vmem_store(
    const isp_ID_t		ID,
    t_vmem_elem		*to,
    const t_vmem_elem	*from,
    unsigned int elems); /* In t_vmem_elem */

void isp_vmem_2d_load(
    const isp_ID_t		ID,
    const t_vmem_elem	*from,
    t_vmem_elem		*to,
    unsigned int height,
    unsigned int width,
    unsigned int stride_to,  /* In t_vmem_elem */

    unsigned		stride_from /* In t_vmem_elem */);

void isp_vmem_2d_store(
    const isp_ID_t		ID,
    t_vmem_elem		*to,
    const t_vmem_elem	*from,
    unsigned int height,
    unsigned int width,
    unsigned int stride_to,  /* In t_vmem_elem */

    unsigned		stride_from /* In t_vmem_elem */);

#endif /* __VMEM_LOCAL_H_INCLUDED__ */
