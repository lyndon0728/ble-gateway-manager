from pathlib import Path
from zipfile import ZipFile
from lxml import etree as E
from PIL import Image, ImageDraw, ImageFont
import json,re,math,os
ROOT=Path('D:/00物联网平台')
FILE=Path(os.environ.get('BLE_VERIFY_FILE',str(ROOT/'outputs/ble-platform-20261107/蓝牙网关平台_需求清单与厂商对标.xlsx')))
OUT=Path(os.environ.get('BLE_QA_OUTPUT',str(ROOT/'research/qa/xlsx')))
OUT.mkdir(parents=True,exist_ok=True)
NS={'m':'http://schemas.openxmlformats.org/spreadsheetml/2006/main'}
z=ZipFile(FILE)
SHEET_COUNT=len(E.fromstring(z.read('xl/workbook.xml')).find('m:sheets',NS))
sty=E.fromstring(z.read('xl/styles.xml'))
fonts=sty.find('m:fonts',NS);fills=sty.find('m:fills',NS);xfs=sty.find('m:cellXfs',NS)
number_formats={int(item.get('numFmtId')):item.get('formatCode','') for item in sty.findall('m:numFmts/m:numFmt',NS)}
def color(x,default='#FFFFFF'):
 if x is None:return default
 return '#'+x.get('rgb','FF'+default.strip('#'))[-6:]
def font(xf,scale=1):
 f=fonts[int(xf.get('fontId','0'))]; sz=float(f.find('m:sz',NS).get('val'))
 bold=f.find('m:b',NS) is not None
 path='C:/Windows/Fonts/msyhbd.ttc' if bold else 'C:/Windows/Fonts/msyh.ttc'
 return ImageFont.truetype(path,round(sz*96/72*scale)),color(f.find('m:color',NS),'#1B2B3E')
def idx(s):
 n=0
 for c in s:n=n*26+ord(c)-64
 return n
def parsecell(s):
 m=re.fullmatch('([A-Z]+)([0-9]+)',s);return idx(m[1]),int(m[2])
def splitlines(text,fo,w):
 lines=[]
 for para in str(text).split('\n'):
  line=''
  for ch in para:
   if line and fo.getlength(line+ch)>w:lines.append(line);line=ch
   else:line+=ch
  lines.append(line)
 return lines
shared=[]
if 'xl/sharedStrings.xml' in z.namelist():
 for si in E.fromstring(z.read('xl/sharedStrings.xml')):
  shared.append(''.join(si.itertext()))
def sheet(n):
 x=E.fromstring(z.read(f'xl/worksheets/sheet{n}.xml'));cols={};rows={};cells={}
 for col in x.findall('m:cols/m:col',NS):
  for i in range(int(col.get('min')),int(col.get('max'))+1):cols[i]=float(col.get('width','8.43'))*7+5
 for row in x.findall('m:sheetData/m:row',NS):
  r=int(row.get('r'));rows[r]=float(row.get('ht','15'))*96/72
  for c in row:
   ci,ri=parsecell(c.get('r'));v=c.find('m:v',NS);txt='' if v is None else v.text or ''
   if c.get('t')=='s':txt=shared[int(txt)]
   if c.get('t')=='inlineStr':txt=''.join(c.find('m:is',NS).itertext())
   si=int(c.get('s','0'))
   if n==4 and ci==3 and ri>=6 and txt:txt='2026-11-07'
   elif c.get('t')=='n' and txt:
    fmt=int(xfs[si].get('numFmtId','0'))
    if number_formats.get(fmt)=='#,##0.00':txt=f'{float(txt):,.2f}'
    elif fmt==200:txt=f'{float(txt):.1f}'
    else:txt=f'{float(txt):.15g}'
   cells[(ci,ri)]=(txt,si)
 return x,cols,rows,cells
over=[]
for n in range(1,SHEET_COUNT+1):
 x,cols,rows,cells=sheet(n)
 for (c,r),(txt,si) in cells.items():
  if not txt:continue
  xf=xfs[si];al=xf.find('m:alignment',NS)
  if al is None or al.get('wrapText')!='1':continue
  fo,_=font(xf);ls=splitlines(txt,fo,cols.get(c,64)-10);lh=fo.getbbox('中文Ag')[3]+2
  need=len(ls)*lh+8
  if need>rows.get(r,20)+2:over.append({'sheet':n,'cell':f'{c},{r}','needed_px':need,'height_px':rows.get(r,20),'sample':txt[:45]})
def render(n,rg,name):
 x,cols,rows,cells=sheet(n);a,b=rg.split(':');c0,r0=parsecell(a);c1,r1=parsecell(b)
 scale=1.4
 ww=math.ceil(sum(cols.get(c,64) for c in range(c0,c1+1))*scale)+20
 hh=math.ceil(sum(rows.get(r,20) for r in range(r0,r1+1))*scale)+20
 im=Image.new('RGB',(ww,hh),'white');dr=ImageDraw.Draw(im);y=10
 # Paint all backgrounds before text so text may overflow empty adjacent cells.
 for r in range(r0,r1+1):
  xx=10;hei=rows.get(r,20)*scale
  for c in range(c0,c1+1):
   w=cols.get(c,64)*scale;_,si=cells.get((c,r),('',0));xf=xfs[si]
   fi=fills[int(xf.get('fillId','0'))].find('m:patternFill/m:fgColor',NS);bg=color(fi)
   if n>1 and r>5 and r%2==0 and bg=='#FFFFFF':bg='#F3F6F9'
   dr.rectangle((xx,y,xx+w,y+hei),fill=bg);xx+=w
  y+=hei
 y=10
 for r in range(r0,r1+1):
  xx=10;hei=rows.get(r,20)*scale
  for c in range(c0,c1+1):
   w=cols.get(c,64)*scale;txt,si=cells.get((c,r),('',0));xf=xfs[si]
   fi=fills[int(xf.get('fillId','0'))].find('m:patternFill/m:fgColor',NS)
   bg=color(fi)
   if n>1 and r>5 and r%2==0 and bg=='#FFFFFF':bg='#F3F6F9'
   if txt:
    fo,fg=font(xf,scale);al=xf.find('m:alignment',NS)
    wrap=al is not None and al.get('wrapText')=='1'
    lines=splitlines(txt,fo,w-10*scale) if wrap else str(txt).split('\n')
    lh=fo.getbbox('中文Ag')[3]+2*scale
    yy=y+4*scale
    if al is not None and al.get('vertical')=='center':yy=y+max(4*scale,(hei-len(lines)*lh)/2)
    for line in lines:
     tx=xx+5*scale
     if al is not None and al.get('horizontal')=='center':tx=xx+(w-fo.getlength(line))/2
     dr.text((tx,yy),line,font=fo,fill=fg);yy+=lh
   xx+=w
  y+=hei
 im.save(OUT/f'{name}.png')
views=[(1,'A1:C38','summary'),(2,'A2:F10','requirements_left'),(2,'H5:J8','requirements_detail'),(2,'K5:T8','requirements_review'),(3,'A2:E10','vendors'),(3,'F5:I7','vendors_logic'),(4,'A2:D8','sources'),(4,'E5:F8','sources_links'),(5,'A2:F9','decisions')]
if os.environ.get('BLE_QA_PROFILE')=='sizing':
 assert SHEET_COUNT==3
 for args in [(1,'A1:F23','inputs'),(1,'A24:F38','results'),(2,'A1:E8','selection'),(3,'A1:E16','acceptance')]:
  render(*args)
 root=sheet(1)[0]
 values={'D27':1500,'D30':1.62,'D31':3000,'D36':2.03,'E27':10000,'E30':27,'E31':20000,'E32':80,'E36':33.75}
 for cell,expected in values.items():
  assert abs(float(root.find(f'.//m:c[@r="{cell}"]/m:v',NS).text)-expected)<0.00001,cell
 for sheet_index in range(1,4):
  root=sheet(sheet_index)[0]
  assert not root.findall('.//m:c[@t="e"]',NS)
  assert root.find('m:sheetViews/m:sheetView/m:pane',NS) is not None
 report={'sheet_count':SHEET_COUNT,'sizing_caches_verified':len(values),'possible_vertical_clipping':over}
 (OUT/'validation.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf8')
 print(json.dumps(report,ensure_ascii=False,indent=2))
 raise SystemExit(0)
if os.environ.get('BLE_VERIFY_FILE'):
 views=[(1,'A1:C42','summary'),(2,'A2:G9','requirements_left'),(2,'H5:J8','requirements_detail'),(2,'K5:Q8','requirements_review'),(3,'A2:D9','policies'),(3,'E5:F9','policies_inputs'),(4,'A2:D8','sources'),(4,'E5:F8','sources_links')]
for args in views:render(*args)
master=sheet(2)[3];ids=[txt for (c,r),(txt,_) in master.items() if c==1 and r>=6 and txt]
expected=int(os.environ.get('BLE_EXPECTED_COUNT','112'))
assert len(ids)==len(set(ids))==expected
for num in range(2,SHEET_COUNT+1):
 x=sheet(num)[0]
 assert x.find('m:tableParts',NS) is not None
 assert x.find('m:sheetViews/m:sheetView/m:pane',NS) is not None
keys=[(2,6),(2,7),(2,8),(2,9)] if os.environ.get('BLE_VERIFY_FILE') else [(2,6),(2,7),(2,8),(2,9),(2,26),(2,27),(2,28)]
vals={c:sheet(1)[3][c][0] for c in keys}
assert float(vals[(2,9)])==expected
assert sum(float(vals[(2,r)]) for r in [6,7,8])==expected
report={'sheet_count':SHEET_COUNT,'requirements':expected,'cached_formula_values':{str(k):v for k,v in vals.items()},'native_preview':'exits with code 1 on a numeric-cell probe; exported OOXML preview used','possible_vertical_clipping':over}
(OUT/'validation.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf8')
print(json.dumps(report,ensure_ascii=False,indent=2))
