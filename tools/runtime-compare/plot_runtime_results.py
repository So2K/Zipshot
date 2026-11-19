"""Publication figures from retained measurements, with scopes and uncertainty.

Uses Matplotlib for reproducible standalone PNG/SVG charts. No timing values
are invented, no failed trials are silently folded into successful samples.
"""
from pathlib import Path
import csv
import hashlib
import json

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib import font_manager
import numpy as np

ROOT=Path(__file__).resolve().parents[2]
DOCS=ROOT/'docs/runtime-comparison'
OUT=ROOT/'media/runtime'
BG,PANEL,INK,MUTED,BLUE,GREY='#13161d','#1c2029','#f4f6fa','#acb5c6','#3a8dff','#b3bccb'


def setup():
    for name in ['Ubuntu-Regular.ttf','Ubuntu-Medium.ttf','Ubuntu-Bold.ttf']:
        font_manager.fontManager.addfont('C:/Windows/Fonts/'+name)
    plt.rcParams.update({'font.family':'Ubuntu','font.size':15,'figure.facecolor':BG,'axes.facecolor':BG,
        'axes.edgecolor':'#495365','axes.labelcolor':MUTED,'text.color':INK,'xtick.color':MUTED,'ytick.color':INK,
        'grid.color':'#343c49','grid.alpha':.65,'savefig.facecolor':BG,'svg.fonttype':'path'})


def save(fig,name):
    fig.savefig(OUT/(name+'.png'),dpi=150)
    fig.savefig(OUT/(name+'.svg'))
    plt.close(fig)


def clean(ax):
    ax.spines[['top','right']].set_visible(False)
    ax.grid(axis='x',zorder=0)
    ax.set_axisbelow(True)


def capture():
    data=json.loads((DOCS/'capture_baseline.json').read_text(encoding='utf-8-sig'))
    groups={(s['app'],s['scenario']):s for s in data['summaries']}
    for group in groups.values():
        if group['measured_trials']!=30 or group['failed_trial_indices']: raise ValueError('Unexpected capture sample count or failures')
    fig,(ax,other)=plt.subplots(1,2,figsize=(13.6,7.6),gridspec_kw={'width_ratios':[1.45,1]})
    fig.subplots_adjust(left=.18,right=.97,top=.70,bottom=.25,wspace=.65)
    fig.text(.055,.94,'From key press to capture',size=28,weight='bold')
    fig.text(.055,.88,'Zipshot 1.0.1 and ShareX 22.4.0  ·  30 measured calls per app and scenario',size=15,color=MUTED)
    from matplotlib.lines import Line2D
    fig.legend(handles=[Line2D([0],[0],color=BLUE,lw=7,label='Zipshot'),Line2D([0],[0],color=GREY,lw=7,label='ShareX 22.4.0')],
        frameon=False,loc='center left',bbox_to_anchor=(.18,.81),ncol=2,fontsize=13)
    for j,(field,label) in enumerate([('clipboard_image_observed','Clipboard image ready'),('closed_wic_image_file_observed','Closed PNG decoded')]):
        for i,(app,color) in enumerate([('zipshot',BLUE),('sharex22',GREY)]):
            row=groups[app,'full']['endpoints'][field];y=1.25-j*.9+(.16 if i==0 else -.16)
            ax.barh(y,row['median_ms'],height=.24,color=color,zorder=3,label='Zipshot' if i==0 else 'ShareX 22.4.0')
            ax.errorbar(row['median_ms'],y,xerr=[[0],[row['p95_ms']-row['median_ms']]],fmt='none',ecolor=INK,capsize=4,lw=1.4,zorder=4)
            ax.text(row['median_ms']+14,y+(.20 if i==0 else -.23),f"{row['median_ms']:.0f} ms",fontsize=15,color=color)
    ax.set_yticks([1.25,.35],['Clipboard image ready','Closed PNG decoded']);ax.set_ylim(-.2,1.8);ax.set_xlim(0,760)
    ax.set_xlabel('First observed readiness, milliseconds');ax.set_title('PrtScn  ·  median and p95',loc='left',size=19,pad=24);clean(ax)
    for i,(app,color,label) in enumerate([('zipshot',BLUE,'Zipshot'),('sharex22',GREY,'ShareX 22.4.0')]):
        row=groups[app,'region']['endpoints']['target_confirmed_visible_response'];y=1-i
        lo,hi=row['median_lower_bound_ms'],row['median_ms']
        other.hlines(y,lo,hi,color=color,lw=16,zorder=3)
        other.plot([lo,hi],[y,y],'|',color=color,ms=24,zorder=4)
        other.text(20,y+.18,f'{lo:.0f} to {hi:.0f} ms',color=color,fontsize=15)
        other.text(20,y-.28,f"p95 upper bound {row['p95_ms']:.0f} ms",color=MUTED,fontsize=11)
    other.set_yticks([1,0],['Zipshot','ShareX 22.4.0']);other.set_ylim(-.6,1.65);other.set_xlim(0,410)
    other.set_xlabel('Readback bracket, ms');other.set_title('Ctrl+PrtScn  ·  visible',loc='left',size=17,pad=24);clean(other)
    fig.text(.055,.145,'Clean Windows Sandbox  ·  2982 × 1672  ·  virtual GPU / Remote Display Adapter',size=14,color=MUTED)
    fig.text(.055,.095,'File and clipboard observations include validation. Visible-response brackets show sampling uncertainty.',size=13,color=MUTED)
    fig.text(.055,.052,'These full-app VM results are separate from the older native-host capture API microbenchmark.',size=13,color=MUTED)
    save(fig,'capture-latency')

    rows=list(csv.DictReader((DOCS/'capture_trials.csv').open(encoding='utf-8-sig',newline='')))
    fig,axes=plt.subplots(3,1,figsize=(13.6,10.3),sharex=True)
    fig.subplots_adjust(left=.10,right=.97,top=.84,bottom=.14,hspace=.45)
    fig.text(.055,.955,'Every measured capture',size=28,weight='bold')
    fig.text(.055,.91,'Three alternating blocks. No recorder was running during these timing samples.',size=15,color=MUTED)
    for ax,(scenario,field,label) in zip(axes,[('full','clipboard_observed_ms','Clipboard image ready'),('full','file_observed_ms','Closed PNG decoded'),('region','visible_upper_bound_ms','Visible response readback')]):
        for app,color,name in [('zipshot',BLUE,'Zipshot'),('sharex22',GREY,'ShareX 22.4.0')]:
            subset=[r for r in rows if r['app']==app and r['scenario']==scenario and r['warmup']=='False' and r['status']=='pass']
            subset.sort(key=lambda r:(int(r['block']),int(r['index'])))
            values=np.array([float(r[field]) for r in subset])
            if len(values)!=30 or min(values)<0:raise ValueError('Incomplete capture trace')
            x=np.arange(1,31)
            ax.plot(x,values,'o-',lw=1.25,ms=4,color=color,label=name)
            if scenario=='region':
                lower=np.array([float(r['visible_lower_bound_ms']) for r in subset]);ax.fill_between(x,lower,values,color=color,alpha=.12)
            ax.axhline(np.median(values),color=color,ls=':',lw=1,alpha=.8)
        ax.set_title(label,loc='left',size=17);ax.set_ylabel('ms');ax.grid(axis='y');ax.spines[['top','right']].set_visible(False)
        for boundary in [10.5,20.5]:ax.axvline(boundary,color='#5b6574',lw=1,ls='--')
    axes[0].legend(frameon=False,loc='upper right',fontsize=12)
    axes[-1].set_xlabel('Measured trial, ordered within each block');axes[-1].set_xlim(.5,30.5);axes[-1].set_xticks([1,5,10,15,20,25,30])
    fig.text(.055,.065,'Shaded visible-response bands are readback brackets, not confidence intervals. All 120 measured calls passed.',size=12,color=MUTED)
    fig.text(.055,.032,'Source data and source hashes are retained in docs/runtime-comparison/capture_baseline.json and capture_trials.csv.',size=12,color=MUTED)
    save(fig,'capture-traces')


def legacy_export():
    data=json.loads((DOCS/'editor_export_18.0.1.json').read_text(encoding='utf-8-sig'))
    summaries={(x['engine'],x['width'],x['case']):x for x in data['summary']}
    print('Export cases',sorted({x['case'] for x in data['summary']}))
    names=[('plain','No annotations'),('arrow','One arrow'),('arrows32','32 arrows'),('freehand','120-point freehand'),('blur_cold','Blur, cold export*'),('blur_warm','Blur, cached export*')]
    for engine in ['zipshot','sharex']:
        for width in [1920,3840]:
            for case,_ in names:
                if (engine,width,case) not in summaries:raise ValueError('Missing export case')
    fig,axes=plt.subplots(1,2,figsize=(13.6,8.4),sharey=True)
    fig.subplots_adjust(left=.18,right=.97,top=.77,bottom=.25,wspace=.16)
    fig.text(.055,.95,'Editor export work',size=28,weight='bold')
    fig.text(.055,.89,'Legacy installed ShareX 18.0.1  ·  production export paths only  ·  30 calls per case',size=15,color=MUTED)
    for ax,width in zip(axes,[1920,3840]):
        for i,(case,label) in enumerate(names):
            for engine,color,dy in [('zipshot',BLUE,.13),('sharex',GREY,-.13)]:
                item=summaries.get((engine,width,case))
                if item is None:continue
                ax.plot(item['median_ms'],5-i+dy,'o',color=color,ms=8)
                ax.hlines(5-i+dy,item['median_ms'],item['p95_ms'],color=color,lw=2)
        ax.set_xscale('log');ax.set_xlim(.3,800);ax.set_ylim(-.6,5.6);ax.set_title('1920 × 1080' if width==1920 else '3840 × 2160',size=18,pad=20)
        ax.set_xlabel('Export time, ms  ·  log scale');clean(ax)
    axes[0].set_yticks(range(5,-1,-1),[label for _,label in names])
    from matplotlib.lines import Line2D
    axes[1].legend(handles=[Line2D([0],[0],marker='o',ls='',color=BLUE,label='Zipshot'),Line2D([0],[0],marker='o',ls='',color=GREY,label='ShareX 18.0.1')],frameon=False,loc='lower right')
    fig.text(.055,.15,'* Blur kernels and caching differ. These cases are not a pixel-equivalent blur-speed contest.',size=13,color=MUTED)
    fig.text(.055,.10,'This is not interactive pointer latency or a comparison with the current ShareX 22 editor.',size=13,color=MUTED)
    fig.text(.055,.05,'Dots are medians, lines extend to p95. PNG encoding and file writing are excluded.',size=13,color=MUTED)
    save(fig,'editor-export-legacy')


def main():
    setup();OUT.mkdir(parents=True,exist_ok=True);capture();legacy_export()
    names=['capture-latency','capture-traces','editor-export-legacy']
    (OUT/'chart-manifest.json').write_text(json.dumps({'data':['docs/runtime-comparison/capture_baseline.json','docs/runtime-comparison/capture_trials.csv','docs/runtime-comparison/editor_export_18.0.1.json'],
        'style':'Matplotlib, local Ubuntu fonts, standalone PNG and SVG with text converted to paths.',
        'sha256':{name+ext:hashlib.sha256((OUT/(name+ext)).read_bytes()).hexdigest() for name in names for ext in ['.png','.svg']}},indent=2)+'\n',encoding='utf-8')

if __name__=='__main__':main()
