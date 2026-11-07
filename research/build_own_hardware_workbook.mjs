import fs from 'node:fs/promises';
import {Workbook,SpreadsheetFile} from '@oai/artifact-tool';
const root='D:/00物联网平台',out=`${root}/outputs/ble-platform-own-gateway-v11`;
const data=JSON.parse(await fs.readFile(`${root}/research/requirements_own_hardware_v11.json`,'utf8'));
const req=data.requirements,end=req.length+5;
const wb=Workbook.create();
const main=wb.worksheets.add('功能规划');const detail=wb.worksheets.add('需求清单');const policy=wb.worksheets.add('交付与服务边界');const source=wb.worksheets.add('参考依据');
const navy='#203B5C',ink='#1B2B3E';
function init(sh,range,title,note){sh.showGridLines=false;sh.getRange(range).format.font={name:'Microsoft YaHei',size:10,color:ink};sh.getRange(range).format.verticalAlignment='top';sh.getRange('A2').values=[[title]];sh.getRange('A2').format.font={bold:true,size:15};sh.getRange('A3').values=[[note]];sh.getRange('A3').format.font={size:10,color:'#55677D'};sh.getRange('A2:T2').format.rowHeight=28;sh.getRange('A3:T3').format.rowHeight=22;}
function tbl(sh,head,rows,widths,name){sh.getRangeByIndexes(4,0,rows.length+1,head.length).values=[head,...rows];const area=sh.getRangeByIndexes(4,0,rows.length+1,head.length);area.format.wrapText=true;sh.getRangeByIndexes(4,0,1,head.length).format={fill:navy,font:{name:'Microsoft YaHei',size:10,bold:true,color:'#FFFFFF'},rowHeight:30,verticalAlignment:'center',horizontalAlignment:'center',wrapText:true};widths.forEach((w,c)=>sh.getRangeByIndexes(0,c,rows.length+5,1).format.columnWidth=w);sh.tables.add(area,true,name).showFilterButton=true;sh.freezePanes.freezeRows(5);rows.forEach((r,i)=>{const lines=Math.max(...r.map((v,j)=>String(v??'').split('\n').reduce((a,t)=>a+Math.max(1,Math.ceil(t.length/(widths[j]*.60))),0)));sh.getRangeByIndexes(i+5,0,1,head.length).format.rowHeight=Math.max(44,lines*17+14);});}
init(main,'A1:D48','自有网关平台功能规划','V1.1  硬件收费  自有网关受管  客户开发应用  不建设定位和行业业务应用');main.tabColor=navy;
main.getRange('A1:A48').format.columnWidth=28;main.getRange('B1:B48').format.columnWidth=17;main.getRange('C1:C48').format.columnWidth=83;
main.getRange('A5:C5').values=[['阶段','需求数','交付目标']];main.getRange('A5:C5').format={fill:navy,font:{bold:true,color:'#FFFFFF'},rowHeight:28};
main.getRange('A6:A9').values=[['P0 首期'],['P1 规模化'],['P2 订单驱动'],['合计']];
main.getRange('B6:B9').formulas=[...['P0','P1','P2'].map(v=>[`=COUNTIFS('需求清单'!$F$6:$F$${end},"${v}")`]),['=SUM(B6:B8)']];
main.getRange('C6:C9').values=[['33条通用首期加2条连接型SKU条件首期，完成交付与接入闭环'],['减少批量交付和售后重复操作，按真实设备规模增强'],['有明确SKU、客户合同和维护预算才启动的能力'],['需求均为设计建议，条件能力按对应SKU首发承诺验收']];
main.getRange('A11:C11').values=[['产品分工','我方负责','客户负责']];main.getRange('A11:C11').format={fill:navy,font:{bold:true,color:'#FFFFFF'},rowHeight:28};
main.getRange('A12:C15').values=[['网关','管理与固件','供电、网络和安装环境'],['蓝牙接入','协议能力边界','终端私有协议及授权'],['数据','传输与调试','业务解析、长期存储与业务数据责任'],['应用','API与SDK','业务规则、界面、工作流与行业服务']];
main.getRange('A18:C18').values=[['推荐模式','阶段','设计目的']];main.getRange('A18:C18').format={fill:navy,font:{bold:true,color:'#FFFFFF'},rowHeight:28};
main.getRange('A19:C21').values=[['我方管理加数据直连','首期默认建议','网关由我方管理，BLE数据直接到客户MQTT或HTTPS目标'],['我方云数据中转','订单驱动','客户需要统一出口时启用，单独约定队列、资源与留存'],['完整客户私有管理','订单驱动','按合同交付；基础本地运行与完全私有平台分别验收']];
main.getRange('A24').values=[['评审状态']];main.getRange('A24').format.font={bold:true,size:12};
const statuses=['待确认','已接受','需修改','暂缓'];main.getRange('A25:A28').values=statuses.map(v=>[v]);main.getRange('B25:B28').formulas=statuses.map(v=>[`=COUNTIFS('需求清单'!$N$6:$N$${end},"${v}")`]);
main.getRange('C25:C28').values=[['条件能力需确认型号及售卖承诺'],['冻结验收前置条件与责任'],['修改原因记录在需求清单P列'],['依赖仍须满足，不能被无声删除']];
main.getRange('A31').values=[['首期优先模块']];main.getRange('A31').format.font={bold:true,size:12};
main.getRange('A32:C38').values=[['设备身份与归属','P0','SN、批次、出货、认领、客户与项目隔离'],['安装与配置','P0','配网、数据目标、配置模板、设备实际生效确认'],['开发者接入','P0','管理API、标准数据格式、凭据、快速入门与样例'],['运行健康','P0','管理连接、BLE采集、客户目标与缓存分别监测'],['维护与升级','P0','远程诊断、脱敏日志、签名网关OTA与型号恢复路径'],['内部售后','基础P0  增强P1','按SN查询版本、批次、故障及RMA关联'],['连接型能力','条件验收','GATT或终端OTA若属于首发SKU销售承诺，则对该SKU首期验收']];
main.getRange('A40').values=[['服务边界：硬件所有权、保修期、云管理服务期、数据保留期分别定义。']];
main.getRange('A41').values=[['默认维持已配置本地采集和客户数据直连；证书、许可等例外必须售前明确并测试。']];
main.getRange('A42').values=[['参考同行的网关管理和开发者服务路径，功能与验收按本公司的硬件及交付条件确定。']];
main.getRange('A5:C38').format.rowHeight=34;main.getRange('A5:C38').format.verticalAlignment='center';main.getRange('A5:C38').format.wrapText=true;main.getRange('A40:A42').format.font={size:9,color:'#55677D'};

init(detail,`A1:Q${end}`,'详细功能需求','只管理自有网关  可透传客户BLE终端  业务解析与长期业务存储由客户承担');
const rows=req.map(r=>[r.id,r.type,r.surface,r.domain,r.title,r.priority,r.phase,r.description,r.condition??r.notes??'按参考型号及冻结协议验收',r.acceptance,r.dependencies.join(', '),r.persona,r.owner_role,r.status,'','',r.source_basis??'设计建议']);
tbl(detail,['需求编号','类型','使用入口','模块','需求名称','优先级','阶段','详细要求','适用条件','验收标准','前置需求','用户角色','责任角色','评审状态','实际负责人','评审意见','依据属性'],rows,[13,12,22,24,30,9,14,70,56,76,36,22,22,14,18,50,24],'OwnGatewayRequirements');detail.freezePanes.freezeColumns(2);
detail.getRange(`N6:N${end}`).dataValidation={rule:{type:'list',values:statuses}};detail.getRange(`N6:P${end}`).format.fill='#FFF5DB';detail.getRange(`F6:F${end}`).conditionalFormats.add('containsText',{text:'P0',format:{font:{bold:true,color:'#9B3C24'},fill:'#FBEDE7'}});

init(policy,'A1:F35','交付与服务边界','黄色列填写正式政策或客户约定  当前不设自助支付和订阅账单功能');
const policies=[
 ['硬件范围','自有网关是受管对象，客户BLE终端按支持协议接入','产品','不接入第三方网关不等于禁止客户终端','',''],
 ['SKU能力','采集型与连接型分别声明；已售卖GATT能力随对应SKU首发验收','产品与固件','没有端侧能力不能靠平台开关兑现','',''],
 ['客户数据目标','首期优先一类现有固件支持的MQTT或HTTPS客户服务器','产品与研发','确认可独立管理并持续直连客户目标','',''],
 ['业务数据责任','客户解析、保存业务数据并开发应用，我方仅必要诊断','客户与产品','明确是否允许短期调试采样和脱敏范围','',''],
 ['硬件所有权','所有权和转移条件按销售合同，与云管理期限分开','销售','设备认领与转移需有安全流程','',''],
 ['硬件保修','起算日期、故障范围、返修换机与运输责任','售后与销售','保修不能用云服务日期代替','',''],
 ['云管理服务期','随硬件包含期限、起算、预警与延续方式','销售与产品','不能默认永久无限资源承诺','',''],
 ['服务到期','远程功能按合同处理，已配置本地/客户直连默认持续','产品与固件','证书或许可例外需售前说明并验证','',''],
 ['日志与调试留存','默认受限窗口，不存全部客户业务历史','产品与运维','建议24小时调试窗口待客户及型号确认','',''],
 ['远程售后授权','按设备、范围和时长授权支持访问','客户与售后','秘密脱敏，关键操作审计','',''],
 ['完全私有管理','完整本地管理部署、升级和支持按订单','解决方案','本地基础运行和私有平台分别定义','',''],
 ['云中转','需要时提供，约定吞吐、出口、队列和留存边界','产品与运维','内部统计成本，当前不自动对客户计费','','']
];tbl(policy,['事项','建议政策','责任角色','需确认的条件','正式决定','确认人'],policies,[22,83,26,76,80,18],'DeliveryPolicies');policy.getRange('E6:F17').format.fill='#FFF5DB';

const all=JSON.parse(await fs.readFile(`${root}/research/all_sources.json`,'utf8'));const chosen=new Set(['S-CAS-02','S-CAS-03','S-MIN-02','S-MQTT-01','S-AWS-01']);const sources=all.filter(x=>chosen.has(x.id));
init(source,'A1:F20','参考依据','官方资料核验日2026年11月7日  产品规划为我方设计建议  不作为竞品实测结论');
tbl(source,['来源编号','资料名称','核验日期','可借鉴的路径或机制','适用边界','原始链接'],sources.map(x=>[x.id,x.title,new Date('2026-11-07T00:00:00Z'),x.claims.join('；'),x.limitations,x.url]),[18,55,16,80,75,115],'OwnGatewaySources');source.getRange('C6:C10').setNumberFormat('yyyy-mm-dd');
wb.recalculate();
console.log((await wb.inspect({kind:'table',range:'功能规划!A5:C9',include:'values,formulas',tableMaxRows:5,tableMaxCols:3,maxChars:2000})).ndjson);
const before=main.getRange('B25').values[0][0];detail.getRange('N6').values=[['已接受']];wb.recalculate();if(main.getRange('B25').values[0][0]!==before-1||main.getRange('B26').values[0][0]!==1)throw new Error('review state recalc failed');detail.getRange('N6').values=[['待确认']];wb.recalculate();
console.log((await wb.inspect({kind:'match',searchTerm:'#REF!|#DIV/0!|#VALUE!|#NAME\\?|#N/A|#NUM!|#NULL!',options:{useRegex:true,maxResults:20},maxChars:1000})).ndjson);
await fs.mkdir(out,{recursive:true});const file=await SpreadsheetFile.exportXlsx(wb);await file.save(`${out}/自有蓝牙网关平台_功能需求清单_V1.1.xlsx`);
await fs.writeFile(`${root}/research/own_workbook_validation.json`,JSON.stringify({requirements:req.length,phaseCounts:Object.fromEntries(['P0','P1','P2'].map(p=>[p,req.filter(r=>r.priority===p).length])),sources:sources.length,recalculation:true,nativeRender:'Known runtime failure; exported OOXML preview used'},null,2));
console.log('saved V1.1 workbook');
