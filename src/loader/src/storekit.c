// Local StoreKit: Apple's store for this (32-bit) version no longer exists. All of the game's in-app purchases
// (21 CI0007_0xx products: costumes, straw dolls, continues, items) complete instantly at no cost,
// using the game's own flow (evacAppPurchase): SKProductsRequest returns the products, addPayment:
// answers "purchased" and restoreCompletedTransactions returns all of them (the game's re-download button).
// Also a minimal NSNumberFormatter (the game uses it to show the price).
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "objc.h"

static const char *g_ids[] = {
  "CI0007_002_UMBRELLA", "CI0007_003_STEEPLEHAT", "CI0007_004_RAGGEDMANTLE", "CI0007_005_WITCHCLOG",
  "CI0007_006_PRINCESSUMBRELLA", "CI0007_007_PRINCESSCROWN", "CI0007_008_PRINCESSDRESS", "CI0007_009_PRINCESSHEEL",
  "CI0007_010_STRAWDOLL_WHITE", "CI0007_011_STRAWDOLL_BLACK", "CI0007_012_STRAWDOLL_GREEN", "CI0007_013_STRAWDOLL_RED",
  "CI0007_014_CONTINUE_2A", "CI0007_015_CONTINUE_2B", "CI0007_016_CONTINUE_5", "CI0007_017_HAMAYA_",
  "CI0007_018_BRISSFUL_", "CI0007_019_RESURECTION", "CI0007_020_GOLD_HORN", "CI0007_021_COTIEGEL",
  "CI0007_022_LUCKY_CHARM",
};
#define NIDS (int)(sizeof(g_ids) / sizeof(g_ids[0]))

static Class_t *getcls(const char *n) { Class_t *c = objc_getClass(n); return c ? c : objc_class_new(n, g_cls_NSObject, 4); }
static Class_t *C_Product, *C_Response, *C_Payment, *C_Trans, *C_Call, *C_Queue, *C_Request, *C_Fmt;

typedef struct { Class_t *isa; id ident, title, desc, price; } Product;
typedef struct { Class_t *isa; id products; } Response;
typedef struct { Class_t *isa; id ident; int qty; } Payment;
typedef struct { Class_t *isa; int state; id payment, tid; } Trans;
typedef struct { Class_t *isa; id obs[4]; int nobs; } Queue;   // iOS notifies ALL observers
typedef struct { Class_t *isa; id ids, delegate; } Request;
typedef struct { Class_t *isa; id target; SEL sel; id a1, a2; int nargs; } Call;   // deferred call on the main loop

// ---- deferred call (StoreKit replies arrive asynchronously on iOS) ----
IMPL static void Call_run(Call *k, SEL c, id unused) {
  dslog("Local StoreKit: -> %s %s", k->target ? class_name(k->target->isa) : "nil", k->sel);
  if (k->target) { if (k->nargs == 2) MSG(k->target, k->sel, k->a1, k->a2); else MSG(k->target, k->sel, k->a1); }
  objc_release_(k->target); objc_release_(k->a1); objc_release_(k->a2);
}
static void later(id target, const char *sel, id a1, id a2, int nargs) {
  Call *k = (Call *)objc_alloc(C_Call, 0);
  k->target = objc_retain_(target); k->sel = sel_registerName(sel); k->a1 = objc_retain_(a1); k->a2 = objc_retain_(a2); k->nargs = nargs;
  runloop_post((id)k, sel_registerName("run:"), NULL);
  objc_release_((id)k);
}
static id str(const char *s) { return objc_autorelease_(nsstring_from_cstr(s)); }
static id arr_new(void) { return MSG((id)objc_getClass("NSMutableArray"), "array"); }

// ---- SKProduct / SKProductsResponse ----
IMPL static id Prod_ident(Product *p, SEL c) { return p->ident; }
IMPL static id Prod_title(Product *p, SEL c) { return p->title; }
IMPL static id Prod_desc(Product *p, SEL c) { return p->desc; }
IMPL static id Prod_price(Product *p, SEL c) { return p->price; }
IMPL static id Prod_locale(Product *p, SEL c) { return NULL; }
IMPL static id Resp_products(Response *r, SEL c) { return r->products; }
IMPL static id Resp_invalid(Response *r, SEL c) { return MSG((id)objc_getClass("NSArray"), "array"); }
static id make_product(const char *ident) {
  Product *p = (Product *)objc_alloc(C_Product, 0);
  p->ident = nsstring_from_cstr(ident);
  char t[64]; const char *n = ident + 11; int j = 0;           // "CI0007_007_PRINCESSCROWN" → "PRINCESSCROWN"
  for (; *n && j < 63; n++) t[j++] = *n == '_' ? ' ' : *n;
  while (j && t[j - 1] == ' ') j--; t[j] = 0;
  p->title = nsstring_from_cstr(t);
  p->desc = nsstring_from_cstr("Unlocked (store closed)");
  p->price = objc_retain_(MSG((id)objc_getClass("NSNumber"), "numberWithInt:", 0));
  return objc_autorelease_((id)p);
}

// ---- SKProductsRequest ----
IMPL static id Req_init(Request *r, SEL c, id ids) { r->ids = objc_retain_(ids); return (id)r; }
IMPL static void Req_setDelegate(Request *r, SEL c, id d) { r->delegate = d; }
IMPL static id Req_delegate(Request *r, SEL c) { return r->delegate; }
IMPL static void Req_start(Request *r, SEL c) {
  Response *resp = (Response *)objc_alloc(C_Response, 0);
  resp->products = objc_retain_(arr_new());
  for (int i = 0; i < NIDS; i++) MSG(resp->products, "addObject:", make_product(g_ids[i]));
  dslog("Local StoreKit: %d products (all free)", NIDS);
  later(r->delegate, "productsRequest:didReceiveResponse:", (id)r, (id)resp, 2);
  if (r->delegate && class_responds(r->delegate->isa, sel_registerName("requestDidFinish:")))
    later(r->delegate, "requestDidFinish:", (id)r, NULL, 1);
  objc_release_((id)resp);
}
IMPL static void Req_cancel(Request *r, SEL c) {}

// ---- SKPayment / SKPaymentTransaction ----
IMPL static id Pay_with(Class_t *cl, SEL c, id ident) {
  Payment *p = (Payment *)objc_alloc(C_Payment, 0); p->ident = MSG(ident, "copy"); p->qty = 1; return objc_autorelease_((id)p);
}
IMPL static id Pay_ident(Payment *p, SEL c) { return p->ident; }
IMPL static int Pay_qty(Payment *p, SEL c) { return p->qty; }
IMPL static int Tr_state(Trans *t, SEL c) { return t->state; }
IMPL static id Tr_payment(Trans *t, SEL c) { return t->payment; }
IMPL static id Tr_id(Trans *t, SEL c) { return t->tid; }
IMPL static id Tr_nil(Trans *t, SEL c) { return NULL; }
IMPL static id Tr_orig(Trans *t, SEL c) { return (id)t; }
IMPL static id Tr_date(Trans *t, SEL c) { return MSG((id)objc_getClass("NSDate"), "date"); }
static int g_tid;
static id make_trans(id payment, int state) {
  Trans *t = (Trans *)objc_alloc(C_Trans, 0);
  t->state = state; t->payment = objc_retain_(payment);
  char b[32]; snprintf(b, sizeof b, "VITA%06d", ++g_tid); t->tid = nsstring_from_cstr(b);
  return objc_autorelease_((id)t);
}

// ---- SKPaymentQueue ----
static Queue *g_queue;
IMPL static id Q_default(Class_t *cl, SEL c) { if (!g_queue) g_queue = (Queue *)objc_alloc(C_Queue, 0); return (id)g_queue; }
IMPL static int Q_can(Class_t *cl, SEL c) { return 1; }
IMPL static void Q_addObs(Queue *q, SEL c, id o) {
  if (!o) return;
  for (int i = 0; i < q->nobs; i++) if (q->obs[i] == o) return;
  if (q->nobs < 4) q->obs[q->nobs++] = o;
  dslog("Local StoreKit: observer %s %p (%d in total)", class_name(o->isa), o, q->nobs);
}
IMPL static void Q_remObs(Queue *q, SEL c, id o) {
  for (int i = 0; i < q->nobs; i++) if (q->obs[i] == o) { q->obs[i] = q->obs[--q->nobs]; break; }
}
static void notify_all(Queue *q, const char *sel, id arg, int nargs) {
  for (int i = 0; i < q->nobs; i++) {
    if (nargs == 2) later(q->obs[i], sel, (id)q, arg, 2);
    else if (class_responds(q->obs[i]->isa, sel_registerName(sel))) later(q->obs[i], sel, (id)q, NULL, 1);
  }
}
IMPL static void Q_add(Queue *q, SEL c, id payment) {
  dslog("Local StoreKit: purchase '%s' -> completed (free)", nsstring_utf8(((Payment *)payment)->ident));
  id a = arr_new(); MSG(a, "addObject:", make_trans(payment, 1 /* SKPaymentTransactionStatePurchased */));
  notify_all(q, "paymentQueue:updatedTransactions:", a, 2);
}
IMPL static void Q_finish(Queue *q, SEL c, id t) {}
IMPL static void Q_restore(Queue *q, SEL c) {
  id a = arr_new();
  for (int i = 0; i < NIDS; i++) MSG(a, "addObject:", make_trans(Pay_with(C_Payment, NULL, str(g_ids[i])), 3 /* Restored */));
  dslog("Local StoreKit: restore -> %d products", NIDS);
  notify_all(q, "paymentQueue:updatedTransactions:", a, 2);
  notify_all(q, "paymentQueueRestoreCompletedTransactionsFinished:", NULL, 1);
}
IMPL static id Q_transactions(Queue *q, SEL c) { return MSG((id)objc_getClass("NSArray"), "array"); }

// ---- minimal NSNumberFormatter ----
typedef struct { Class_t *isa; int style; } Fmt;
IMPL static id Fmt_init(Fmt *f, SEL c) { return (id)f; }
IMPL static void Fmt_setStyle(Fmt *f, SEL c, int s) { f->style = s; }
IMPL static void Fmt_nop(Fmt *f, SEL c, id x) {}
IMPL static id Fmt_string(Fmt *f, SEL c, id num) {
  if (f->style == 2) return str("Free");                               // NSNumberFormatterCurrencyStyle
  long v = num ? (long)MSG(num, "intValue") : 0; char b[32]; snprintf(b, sizeof b, "%ld", v); return str(b);
}

void storekit_init(void) {
  C_Call = objc_class_new("VitaDeferredCall", g_cls_NSObject, sizeof(Call)); class_add(C_Call, "run:", Call_run);
  C_Product = getcls("SKProduct"); C_Response = getcls("SKProductsResponse"); C_Payment = getcls("SKPayment");
  C_Trans = getcls("SKPaymentTransaction"); C_Queue = getcls("SKPaymentQueue"); C_Request = getcls("SKProductsRequest");
  C_Fmt = getcls("NSNumberFormatter");
  // objects are created with objc_alloc(cls, 0): the instance size must be large enough for the fields
  extern void class_set_instance_size(Class_t *, uint32_t);
  class_set_instance_size(C_Product, sizeof(Product)); class_set_instance_size(C_Response, sizeof(Response));
  class_set_instance_size(C_Payment, sizeof(Payment)); class_set_instance_size(C_Trans, sizeof(Trans));
  class_set_instance_size(C_Queue, sizeof(Queue)); class_set_instance_size(C_Request, sizeof(Request));
  class_set_instance_size(C_Fmt, sizeof(Fmt));
  class_add(C_Product, "productIdentifier", Prod_ident); class_add(C_Product, "localizedTitle", Prod_title);
  class_add(C_Product, "localizedDescription", Prod_desc); class_add(C_Product, "price", Prod_price);
  class_add(C_Product, "priceLocale", Prod_locale);
  class_add(C_Response, "products", Resp_products); class_add(C_Response, "invalidProductIdentifiers", Resp_invalid);
  class_add(C_Request, "initWithProductIdentifiers:", Req_init); class_add(C_Request, "setDelegate:", Req_setDelegate);
  class_add(C_Request, "delegate", Req_delegate); class_add(C_Request, "start", Req_start); class_add(C_Request, "cancel", Req_cancel);
  class_add_meta(C_Payment, "paymentWithProductIdentifier:", Pay_with);
  class_add(C_Payment, "productIdentifier", Pay_ident); class_add(C_Payment, "quantity", Pay_qty);
  class_add(C_Trans, "transactionState", Tr_state); class_add(C_Trans, "payment", Tr_payment);
  class_add(C_Trans, "transactionIdentifier", Tr_id); class_add(C_Trans, "error", Tr_nil);
  class_add(C_Trans, "transactionReceipt", Tr_nil); class_add(C_Trans, "originalTransaction", Tr_orig);
  class_add(C_Trans, "transactionDate", Tr_date);
  class_add_meta(C_Queue, "defaultQueue", Q_default); class_add_meta(C_Queue, "canMakePayments", Q_can);
  class_add(C_Queue, "addTransactionObserver:", Q_addObs); class_add(C_Queue, "removeTransactionObserver:", Q_remObs);
  class_add(C_Queue, "addPayment:", Q_add); class_add(C_Queue, "finishTransaction:", Q_finish);
  class_add(C_Queue, "restoreCompletedTransactions", Q_restore); class_add(C_Queue, "transactions", Q_transactions);
  class_add(C_Fmt, "init", Fmt_init); class_add(C_Fmt, "setNumberStyle:", Fmt_setStyle);
  class_add(C_Fmt, "setFormatterBehavior:", Fmt_nop); class_add(C_Fmt, "setLocale:", Fmt_nop);
  class_add(C_Fmt, "stringFromNumber:", Fmt_string);
}
