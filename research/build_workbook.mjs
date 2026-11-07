import fs from 'node:fs/promises';
import { Workbook, SpreadsheetFile } from '@oai/artifact-tool';
const root='D:/00物联网平台';
const out=`${root}/outputs/ble-platform-20261107`;
const req=(JSON.parse(await fs.readFile(`${root}/research/requirements.json`,'utf8'))).requirements;
const vendors=(JSON.parse(await fs.readFile(`${root}/research/vendor_supplement.json`,'utf8'))).vendors;
const sources=JSON.parse(await fs.readFile(`${root}/research/all_sources.json`,'utf8'));
const data=JSON.parse(await fs.readFile(`${root}/research/requirements.json`,'utf8'));
const wb=Workbook.create();
const summary=wb.worksheets.add('需求总览');
const requirements=wb.worksheets.add('需求清单');
const comparison=wb.worksheets.add('厂商对标');
const evidence=wb.worksheets.add('证据与来源');
const decisions=wb.worksheets.add('待决事项');
const navy='#203B5C', blue='#EAF1F8', ink='#1B2B3E';
function init(sh,range){sh.showGridLines=false;sh.getRange(range).format.font={name:'Microsoft YaHei',size:10,color:ink};sh.getRange(range).format.verticalAlignment='top';}
function col(sh,letter,w){sh.getRange(`${letter}1:${letter}200`).format.columnWidth=w;}
function title(sh,t,note){sh.getRange('A2').values=[[t]];sh.getRange('A2').format.font={bold:true,size:15,color:ink};sh.getRange('A3').values=[[note]];sh.getRange('A3').format.font={size:10,italic:true,color:'#55677D'};sh.getRange('A2:A3').format.wrapText=false;sh.getRange('A2:T2').format.rowHeight=28;sh.getRange('A3:T3').format.rowHeight=22;}
function table(sh,headers,rows,name,widths){
 const n=rows.length+5, m=headers.length;
 sh.getRangeByIndexes(4,0,rows.length+1,m).values=[headers,...rows];
 sh.getRangeByIndexes(4,0,rows.length+1,m).format.wrapText=true;
 sh.getRangeByIndexes(4,0,1,m).format={fill:navy,font:{bold:true,color:'#FFFFFF',name:'Microsoft YaHei',size:10},rowHeight:30,verticalAlignment:'center',horizontalAlignment:'center',wrapText:true};
 widths.forEach((w,i)=>sh.getRangeByIndexes(0,i,n,1).format.columnWidth=w);
 const tab=sh.tables.add(sh.getRangeByIndexes(4,0,rows.length+1,m),true,name);
 tab.showFilterButton=true;
 sh.freezePanes.freezeRows(5);
 for(let i=0;i<rows.length;i++){
  const lines=Math.max(...rows[i].map((v,j)=>Math.ceil(String(v??'').length/(widths[j]*0.70))));
  sh.getRangeByIndexes(i+5,0,1,m).format.rowHeight=Math.max(40, Math.min(160,lines*17+16));
 }
 return n;
}
init(summary,'A1:J45');summary.tabColor=navy;title(summary,'蓝牙网关平台需求总览','2026年11月7日  企业级BLE  自研商用多租户假设  所有需求待范围评审');
col(summary,'A',32);col(summary,'B',17);col(summary,'C',68);col(summary,'D',3);
summary.getRange('A5:C5').values=[['优先级','需求数','范围定义']];
summary.getRange('A5:C5').format={fill:navy,font:{bold:true,color:'#FFFFFF'},rowHeight:28};
summary.getRange('A6:A9').values=[['P0'],['P1'],['P2'],['合计']];
const end=req.length+5;
summary.getRange('B6:B9').formulas=[...['P0','P1','P2'].map(x=>[`=COUNTIFS('需求清单'!$E$6:$E$${end},"${x}")`]),['=SUM(B6:B8)']];
summary.getRange('C6:C9').values=[['企业商用试点基线，含首批功能与投产门槛'],['增强能力，按设备和客户场景启用'],['生态与专业引擎扩展'],['112条：22条业务，90条功能、技术和非功能']];
summary.getRange('A11:C11').values=[['评审状态','数量','使用说明']];
summary.getRange('A11:C11').format={fill:navy,font:{bold:true,color:'#FFFFFF'},rowHeight:28};
const statuses=['待确认','已接受','需修改','暂缓'];
summary.getRange('A12:A15').values=statuses.map(x=>[x]);
summary.getRange('B12:B15').formulas=statuses.map(x=>[`=COUNTIFS('需求清单'!$P$6:$P$${end},"${x}")`]);
summary.getRange('C12:C15').values=[['需求清单P列可编辑评审状态；Q列可填实际负责人'],['接受后冻结验收标准与设备/资源条件'],['保留原因并修改后重新评审'],['暂缓不得阻塞已接受的前置需求']];
summary.getRange('A18').values=[['容量计算示例']];summary.getRange('A18').format.font={bold:true,size:12};
summary.getRange('A19:C24').values=[['参数','输入值','单位与含义'],['终端数量',10000,'台'],['广播周期',10,'秒；每终端每周期一条广播'],['平均接收网关数',2,'个；多网关观测分别保留'],['归一事件大小',500,'字节；不含索引、副本与原始包'],['示例载荷留存',7,'天；估算窗口，不替代原始及归一数据分层策略']];
summary.getRange('A19:C19').format={fill:navy,font:{bold:true,color:'#FFFFFF'},rowHeight:28};
summary.getRange('B20:B24').format.fill='#FFF1CA';
summary.getRange('A26:A28').values=[['观测速率'],['每日纯事件载荷'],['留存期纯事件载荷']];
summary.getRange('B26:B28').formulas=[['=B20/B21*B22'],['=B26*B23*86400/1000000000'],['=B27*B24']];
summary.getRange('C26:C28').values=[['条/秒；只表示模型观测量，不是实测射频接收率'],['GB/天，十进制；待测压缩和过滤比例'],['GB；存储预算另加索引、副本、队列和安全余量']];
summary.getRange('B27:B28').setNumberFormat('0.0');
summary.getRange('A31').values=[['评审顺序']];summary.getRange('A31').format.font={bold:true,size:12};
summary.getRange('A32:C35').values=[['第一步','业务范围','先确认首发场景、客户角色、交付模式与参考设备'],['第二步','验收基线','批准容量模型、设备条件、业务时效与恢复目标'],['第三步','分期依赖','按需求依赖安排，P0内区分首批功能与投产检查'],['第四步','采购验证','厂商对标是公开事实；自建需求均为设计建议']];
summary.getRange('A37').values=[['来源说明：厂商公开事实在厂商对标及证据与来源，需求中的来源编号只表示推导依据。']];
summary.getRange('A38').values=[['文档：配套BRD定义业务流程与阶段，TRD定义架构、接口、容量和AT01至AT12组合验收。']];
summary.getRange('A5:C35').format.rowHeight=29;summary.getRange('A5:C35').format.verticalAlignment='center';
summary.getRange('C5:C35').format.wrapText=true;summary.getRange('A37:A38').format.font={size:9,color:'#55677D'};

function n(r){return +r.id.split('-')[1];}
function refs(r){
 if(r.id.startsWith('BR')) return n(r)===18?'S-JJ-01, S-QUU-01, S-SIG-01': r.domain.includes('集成')?'S-CAS-02, S-CIS-01':'S-CAS-01, S-KON-01, S-MIN-01';
 let i=n(r);return i<=20?'S-CAS-01, S-AWS-01':i<=30?'S-CAS-02, S-CAS-03, S-SIG-01':i<=40?'S-MQTT-01, S-QUU-02':i<=50?'S-AWS-02, S-MOKO-01':i<=60?'S-KON-01, S-JJ-01, S-QUU-02':i<=70?'S-KON-02, S-BLUEUP-01':i<=80?'S-CAS-02, S-CIS-01, S-ARU-01':'S-MQTT-01, S-AWS-03';
}
function trace(r){
 if(r.id.startsWith('BR'))return ['BRD一至八','TRD一与十二','范围评审及UC01至UC06'];
 let i=n(r);return i<=10?['BRD四与六','TRD三与九','AT01, AT12']:i<=20?['BRD五与六','TRD三四七','AT05, AT12']:i<=30?['BRD三与六','TRD四六','AT07, AT08']:i<=40?['BRD五与六','TRD五十','AT02, AT03, AT04']:i<=50?['BRD五UC04与UC05','TRD五七十一','AT02, AT05, AT06']:i<=60?['BRD五UC02','TRD三六','AT03, AT08']:i<=70?['BRD五UC03','TRD八','AT09']:i<=80?['BRD五UC06与七','TRD八十一','AT01, AT11, AT12']:['BRD七与八','TRD九十十一十二','AT01, AT04, AT10, AT12'];
}
init(requirements,`A1:T${end}`);title(requirements,'详细需求清单','来源启发不等于厂商能力声明  P列为评审状态  Q列填写实际负责人  全部数值为待批准建议');
const reqRows=req.map(r=>{const tr=trace(r);return [r.id,r.type,r.domain,r.title,r.priority,r.phase,r.persona,r.description,r.value,r.acceptance,r.dependencies.join(', '),refs(r),'设计建议；来源仅为启发',r.owner_role,tr[0]+'；'+tr[1],r.status,'',tr[2],'',''];});
table(requirements,['需求编号','类型','模块','需求名称','优先级','阶段','使用角色','详细要求','业务价值','验收标准','前置需求','来源编号','证据属性','责任角色','文档章节','评审状态','实际负责人','组合验收参考','评审意见','计划版本'],reqRows,'Requirements',[13,10,18,26,9,10,18,65,31,68,32,40,30,18,29,13,16,32,40,15]);
requirements.freezePanes.freezeColumns(4);
requirements.getRange(`P6:P${end}`).dataValidation={rule:{type:'list',values:statuses}};
requirements.getRange(`E6:E${end}`).dataValidation={rule:{type:'list',values:['P0','P1','P2']}};
requirements.getRange(`P6:Q${end}`).format.fill='#FFF5DB';
requirements.getRange(`S6:T${end}`).format.fill='#FFF5DB';
requirements.getRange(`E6:E${end}`).conditionalFormats.add('containsText',{text:'P0',format:{font:{bold:true,color:'#9B3C24'},fill:'#FBEDE7'}});

const compRows=[];
for(const v of vendors)for(const f of v.features)compRows.push([v.name,v.route,f[0],f[1],f[2],v.verify,v.logic,v.implication,v.refs]);
init(comparison,`A1:I${compRows.length+5}`);title(comparison,'厂商能力与构建逻辑','9家代表厂商  不作全球排名  已证实表示官网明确描述  未披露不代表不支持');
table(comparison,['厂商与平台','产品路线','能力维度','公开能力或边界','核验结论','需要验证的限制','构建逻辑分析','需求推导','来源编号'],compRows,'VendorEvidence',[32,22,16,54,21,65,62,65,42]);comparison.freezePanes.freezeColumns(2);

const sourceRows=sources.map(x=>[x.id,x.title,new Date('2026-11-07T00:00:00Z'),Array.isArray(x.claims)?x.claims.join('；'):x.claims,x.limitations,x.url]);
init(evidence,`A1:F${sourceRows.length+5}`);title(evidence,'证据与来源','一手厂商官网及官方技术资料  核验日2026年11月7日  适用边界与网页链接逐项保留');
table(evidence,['来源编号','资料名称','核验日期','支持的事实','适用边界','原始链接'],sourceRows,'Sources',[18,48,16,75,70,105]);evidence.getRange(`C6:C${sourceRows.length+5}`).setNumberFormat('yyyy-mm-dd');evidence.freezePanes.freezeColumns(1);

const qs=data.open_questions.map((q,i)=>[q.id??i+1,typeof q==='string'?q:q.question+'\n影响：'+q.impact,q.suggested_owner??'产品与技术负责人','待确认','','']);
init(decisions,`A1:F${qs.length+5}`);title(decisions,'待决事项','这些问题用于下一轮范围冻结  填写决策不自动等于需求已验收');
table(decisions,['序号','关键问题','建议责任角色','状态','决策与依据','决策人'],qs,'Decisions',[9,100,26,15,90,18]);
decisions.getRange(`D6:D${qs.length+5}`).dataValidation={rule:{type:'list',values:['待确认','已决定','需试点','暂缓']}};
decisions.getRange(`D6:F${qs.length+5}`).format.fill='#FFF5DB';

wb.recalculate();
console.log((await wb.inspect({kind:'table',range:'需求总览!A5:C15',include:'values,formulas',tableMaxRows:12,tableMaxCols:3,maxChars:3500})).ndjson);
const initial=summary.getRange('B26').values[0][0];summary.getRange('B20').values=[[20000]];wb.recalculate();const changed=summary.getRange('B26').values[0][0];summary.getRange('B20').values=[[10000]];wb.recalculate();
if(initial!==2000||changed!==4000||summary.getRange('B26').values[0][0]!==2000)throw new Error('capacity recalculation failed');
console.log((await wb.inspect({kind:'match',searchTerm:'#REF!|#DIV/0!|#VALUE!|#NAME\\?|#N/A|#NUM!|#NULL!|#SPILL!|#CALC!',options:{useRegex:true,maxResults:30},summary:'final formula scan',maxChars:1500})).ndjson);
await fs.mkdir(`${root}/research/qa/xlsx`,{recursive:true});
// Native artifact preview exits on this Windows runtime even for one numeric cell.
// Use exported OOXML dimensions/styles for the task-local fallback QA renderer.
for(const [name,range,file] of (process.env.BLE_NATIVE_RENDER==='1'?[['需求总览','A1:C38','summary'],['需求清单','A2:F10','requirements_left'],['需求清单','H5:J8','requirements_detail'],['需求清单','K5:T8','requirements_review'],['厂商对标','A2:E10','vendors'],['厂商对标','F5:I7','vendors_logic'],['证据与来源','A2:D8','sources'],['证据与来源','E5:F8','sources_links'],['待决事项','A2:F9','decisions']]:[])){
 console.log('rendering '+file);
 const blob=await wb.render({sheetName:name,range,scale:1.3,format:'png'});await fs.writeFile(`${root}/research/qa/xlsx/${file}.png`,new Uint8Array(await blob.arrayBuffer()));
}
const file=await SpreadsheetFile.exportXlsx(wb);await file.save(`${out}/蓝牙网关平台_需求清单与厂商对标.xlsx`);
await fs.writeFile(`${root}/research/qa/workbook_validation.json`,JSON.stringify({requirements:req.length,sources:sources.length,vendors:vendors.length,comparison_rows:compRows.length,capacity_recalculation:true},null,2));
console.log('saved workbook');
